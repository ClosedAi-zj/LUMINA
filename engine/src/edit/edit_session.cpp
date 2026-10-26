// 编辑会话实现
#include "edit/edit_session.h"

#include "core/logger.h"
#include "core/str_util.h"

namespace lumina {
namespace edit {

EditSessionManager::EditSessionManager(xmp::XmpService* xmp, db::PhotoDao* photos)
    : xmp_(xmp),
      photos_(photos),
      developSettings_(photos ? photos->database() : nullptr),
      history_(photos ? photos->database() : nullptr),
      running_(true) {}

EditSessionManager::~EditSessionManager() { shutdown(); }

std::shared_ptr<SessionState> EditSessionManager::findSession(int64_t sessionId) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = sessions_.find(sessionId);
  if (it == sessions_.end()) return nullptr;
  return it->second;
}

json::JsonValue EditSessionManager::begin(int64_t photoId, rpc::RpcError* err) {
  json::JsonValue params;
  std::string loadErr;
  if (!xmp_->loadParams(photoId, &params, &loadErr)) {
    if (loadErr.find("xmp parse failed") != std::string::npos) {
      *err = rpc::RpcError(rpc::kXmpParseError, loadErr);
    } else {
      *err = rpc::RpcError(rpc::kFileNotFound, loadErr);
    }
    return json::JsonValue::nullValue();
  }
  auto session = std::make_shared<SessionState>();
  session->photoId = photoId;
  session->params = params;
  session->lastChangeMs = nowMs();
  session->dirty = false;

  // ---- T2.4 跨会话历史恢复（架构 §4.2）----
  // 持久化历史 = base（最近 checkpoint 的完整参数）+ Σdiff；sidecar 是参数唯一真相源。
  // 重放结果与 sidecar 参数指纹一致才采用；否则整段丢弃并显式 warning（零静默）。
  std::string histWarning;
  bool historyRestored = false;
  {
    db::HistoryStateRow st;
    std::string hErr;
    const bool stateOk = history_.state(photoId, &st, &hErr);
    if (!stateOk && !hErr.empty()) {
      logWarn("edit", "history state 读取失败（派生层，本次会话仅内存 undo/redo）: " + hErr);
    }
    if (stateOk && st.exists) {
      json::JsonValue base;
      std::string pErr;
      bool replayable = json::JsonValue::parse(st.baseJson, &base, &pErr);
      std::vector<db::HistoryEntryRow> rows;
      std::string lErr;
      if (replayable && !history_.list(photoId, &rows, &lErr)) replayable = false;
      json::JsonValue ops = json::JsonValue::arrayValue();
      if (replayable) {
        for (const auto& row : rows) {
          if (row.seq > st.currentSeq) continue;  // redo 分支不跨会话（HistoryStack.push 同语义）
          json::JsonValue op;
          if (!json::JsonValue::parse(row.opJson, &op, &pErr)) {
            replayable = false;
            break;
          }
          ops.addItem(op);
        }
      }
      if (replayable) {
        json::JsonValue replayed;
        if (applyOps(base, ops, &replayed) &&
            db::DevelopSettingsDao::hashParams(replayed) ==
                db::DevelopSettingsDao::hashParams(params) &&
            session->history.restoreFrom(base, ops, static_cast<int>(st.currentSeq))) {
          historyRestored = true;
        }
      }
      if (!historyRestored) {
        // 外部改过 sidecar / 崩溃后 sidecar 落后 / undo 穿越折叠点 → 显式重置
        histWarning = "历史记录与当前参数不一致，已重置";
        logWarn("edit", "photo " + std::to_string(photoId) + ": " + histWarning);
        std::string cErr;
        if (!history_.clear(photoId, &cErr)) logWarn("edit", "history 重置失败: " + cErr);
      }
    }
    if (!historyRestored) {
      session->history.push(params);
      std::string iErr;
      if (!history_.initBase(photoId, params, &iErr)) {
        logWarn("edit", "history base 初始化失败（派生层，不影响编辑）: " + iErr);
      }
    }
  }
  {
    std::lock_guard<std::mutex> lock(mutex_);
    session->sessionId = nextSessionId_++;
    sessions_[session->sessionId] = session;
  }
  // M2 T2.3：打开照片时做一次缓存一致性巡检（§3.3.2 的四行场景表）。
  // 参数本身永远来自 sidecar；缓存坏了/旧了就地自愈，sidecar 缺失则显式提示。
  std::string cacheState = "unavailable";
  json::JsonValue cacheWarnings = json::JsonValue::arrayValue();
  {
    PhotoRecord rec;
    std::string recErr;
    if (photos_->getById(photoId, &rec, &recErr)) {
      const std::string sidecar =
          rec.sidecarPath.empty() ? xmp_->sidecarPathFor(rec.filePath) : rec.sidecarPath;
      const bool exists = fileExists(sidecar);
      db::ReconcileResult rr;
      std::string reErr;
      if (developSettings_.reconcile(photoId, params, fileMtimeMs(sidecar), exists, &rr, &reErr)) {
        cacheState = rr.state;
        if (!rr.warning.empty()) cacheWarnings.addItem(rr.warning);
      } else {
        // 缓存层失败绝不影响编辑（参数来自 sidecar），但必须留痕，不得静默
        cacheState = "error";
        cacheWarnings.addItem("参数缓存巡检失败：" + reErr);
        logWarn("edit", "develop_settings reconcile failed for photo " + std::to_string(photoId) +
                            ": " + reErr);
      }
    }
  }
  json::JsonValue out = json::JsonValue::objectValue();
  out.setItem("sessionId", session->sessionId);
  out.setItem("params", session->params);
  out.setItem("revision", session->history.index());
  // M18：historyLength 取持久化口径（恢复后 = current_seq+1，含折叠点前的灰字虚拟步）
  out.setItem("historyLength", session->history.totalSteps());
  out.setItem("historyRestored", historyRestored);
  out.setItem("cacheState", cacheState);
  if (!histWarning.empty()) cacheWarnings.addItem(histWarning);
  out.setItem("warnings", cacheWarnings);
  logInfo("edit", "session " + std::to_string(session->sessionId) + " begin for photo " +
                      std::to_string(photoId));
  return out;
}

json::JsonValue EditSessionManager::setParams(int64_t sessionId, const json::JsonValue& patch,
                                              bool commitHistory, rpc::RpcError* err) {
  auto session = findSession(sessionId);
  if (!session) {
    *err = rpc::RpcError(rpc::kInvalidParams, "session not found: " + std::to_string(sessionId));
    return json::JsonValue::nullValue();
  }
  const json::JsonValue& p = patch.get("patch");
  if (!p.isObject()) {
    *err = rpc::RpcError(rpc::kInvalidParams, "params.patch must be an object");
    return json::JsonValue::nullValue();
  }
  {
    std::lock_guard<std::mutex> lock(mutex_);
    const json::JsonValue before = session->params;
    session->params.mergePatch(p);
    session->lastChangeMs = nowMs();
    session->dirty = true;
    if (commitHistory) {
      // T2.4：diff op 落盘（单 op 一条事务 ≤1ms；每 kFoldEvery 步折叠为 checkpoint）。
      // 持久化失败只 logWarn——内存 undo/redo 与 sidecar 参数不受影响（可丢弃派生口径）。
      std::string label;
      const json::JsonValue op = diffOps(before, session->params, &label);
      session->history.pushCommitted(session->params, op);  // op=null（无差异）时等价 push
      if (!op.isNull()) {
        int64_t seq = 0;
        std::string hErr;
        if (history_.append(session->photoId, op, label, sessionId, &seq, &hErr)) {
          if (seq > 0 && seq % db::HistoryDao::kFoldEvery == 0) {
            std::string cpErr;
            if (history_.checkpoint(session->photoId, seq, session->params, &cpErr)) {
              logInfo("edit", "history checkpoint: photo " + std::to_string(session->photoId) +
                                  " 折叠于 seq " + std::to_string(seq));
            } else {
              logWarn("edit", "history 折叠失败（派生层，不影响编辑）: " + cpErr);
            }
          }
        } else {
          logWarn("edit", "history append 失败（派生层，内存 undo/redo 不受影响）: " + hErr);
        }
      }
    } else {
      // 滑块拖动中间态：覆盖当前栈顶，不新增历史
      session->history.replaceCurrent(session->params);
    }
  }
  json::JsonValue out = json::JsonValue::objectValue();
  out.setItem("params", session->params);
  out.setItem("revision", session->history.index());
  out.setItem("historyLength", session->history.totalSteps());
  return out;
}

json::JsonValue EditSessionManager::undo(int64_t sessionId, rpc::RpcError* err) {
  auto session = findSession(sessionId);
  if (!session) {
    *err = rpc::RpcError(rpc::kInvalidParams, "session not found");
    return json::JsonValue::nullValue();
  }
  json::JsonValue previous;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!session->history.undo(&previous)) {
      *err = rpc::RpcError(rpc::kInvalidParams, "nothing to undo");
      return json::JsonValue::nullValue();
    }
    session->params = previous;
    session->dirty = true;
    session->lastChangeMs = nowMs();
    // T2.4：持久化游标同步（current_seq = virtualBase + index；undo 向后移动）
    std::string hErr;
    if (!history_.updateSeq(session->photoId,
                            session->history.virtualBase() + session->history.index(), &hErr)) {
      logWarn("edit", "history 游标更新失败（派生层，内存 undo/redo 不受影响）: " + hErr);
    }
  }
  json::JsonValue out = json::JsonValue::objectValue();
  out.setItem("params", session->params);
  out.setItem("revision", session->history.index());
  out.setItem("historyIndex", session->history.index());
  out.setItem("historyLength", session->history.totalSteps());
  return out;
}

json::JsonValue EditSessionManager::redo(int64_t sessionId, rpc::RpcError* err) {
  auto session = findSession(sessionId);
  if (!session) {
    *err = rpc::RpcError(rpc::kInvalidParams, "session not found");
    return json::JsonValue::nullValue();
  }
  json::JsonValue next;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!session->history.redo(&next)) {
      *err = rpc::RpcError(rpc::kInvalidParams, "nothing to redo");
      return json::JsonValue::nullValue();
    }
    session->params = next;
    session->dirty = true;
    session->lastChangeMs = nowMs();
    // T2.4：持久化游标同步（redo 向前移动）
    std::string hErr;
    if (!history_.updateSeq(session->photoId,
                            session->history.virtualBase() + session->history.index(), &hErr)) {
      logWarn("edit", "history 游标更新失败（派生层，内存 undo/redo 不受影响）: " + hErr);
    }
  }
  json::JsonValue out = json::JsonValue::objectValue();
  out.setItem("params", session->params);
  out.setItem("revision", session->history.index());
  out.setItem("historyIndex", session->history.index());
  out.setItem("historyLength", session->history.totalSteps());
  return out;
}

bool EditSessionManager::flushSession(const std::shared_ptr<SessionState>& session,
                                      std::string* err) {
  // 写序铁律（架构 §3.3.2）：先原子写 sidecar，成功后才更新派生缓存。
  if (!xmp_->saveParams(session->photoId, session->params, err)) return false;
  // 缓存是可丢弃派生：写失败只 warning + UI 状态点，**绝不回滚 sidecar**。
  PhotoRecord rec;
  std::string dbErr;
  int64_t mtime = 0;
  if (photos_->getById(session->photoId, &rec, &dbErr)) mtime = rec.sidecarMtime;
  std::string cacheErr;
  if (!developSettings_.upsert(session->photoId, session->params, mtime, "xmp", &cacheErr)) {
    logWarn("edit", "develop_settings 缓存写入失败（sidecar 已成功落盘，不回滚）: photo " +
                        std::to_string(session->photoId) + " " + cacheErr);
  }
  return true;
}

// 结束会话（ipc-protocol v1.0.1 §5.5 唯一定义）：
//   flush=true （默认，关图/切图/退出修图台必传）：跳过 500ms 防抖立即原子写落盘，
//     销毁会话，返回 {flushed, params, revision, sidecarMtime}；UI 凭 sidecarMtime
//     熄灭「参数已写入 XMP」指示灯。若会话无未落盘变更（dirty=false），sidecar 本就
//     与内存一致，无需重写，直接返回其当前 mtime（避免无谓地制造新 mtime）。
//   flush=false：仅销毁内存会话、不落盘，丢弃防抖窗口内未落盘的 patch
//     （仅用于用户显式放弃本次编辑；已落盘的历史步骤不受影响）。
json::JsonValue EditSessionManager::end(int64_t sessionId, bool flush, rpc::RpcError* err) {
  auto session = findSession(sessionId);
  if (!session) {
    *err = rpc::RpcError(rpc::kInvalidParams, "session not found");
    return json::JsonValue::nullValue();
  }
  bool flushed = false;
  bool haveMtime = false;
  int64_t sidecarMtime = 0;
  if (flush) {
    if (session->dirty) {
      std::string flushErr;
      if (!flushSession(session, &flushErr)) {
        // 落盘失败（含外部修改冲突）不销毁会话，用户可重试或显式放弃
        *err = rpc::RpcError(rpc::kXmpWriteFailed, flushErr);
        return json::JsonValue::nullValue();
      }
      std::lock_guard<std::mutex> lock(mutex_);
      session->dirty = false;
    }
    // 取 sidecar 当前 mtime（刚写完，或本就一致）；sidecar 不存在时为 null
    PhotoRecord rec;
    std::string dbErr;
    if (photos_->getById(session->photoId, &rec, &dbErr)) {
      std::string sidecar =
          rec.sidecarPath.empty() ? xmp_->sidecarPathFor(rec.filePath) : rec.sidecarPath;
      int64_t m = fileMtimeMs(sidecar);
      if (m > 0) {
        sidecarMtime = m;
        haveMtime = true;
      }
    }
    flushed = true;
  }
  json::JsonValue out = json::JsonValue::objectValue();
  out.setItem("flushed", flushed);
  out.setItem("params", session->params);
  out.setItem("revision", session->history.index());
  out.setItem("sidecarMtime",
              haveMtime ? json::JsonValue::integer(sidecarMtime) : json::JsonValue::nullValue());
  logInfo("edit", "session " + std::to_string(sessionId) + " ended (flush=" +
                      (flush ? "true" : "false") + ", flushed=" + (flushed ? "true" : "false") +
                      ")");
  {
    std::lock_guard<std::mutex> lock(mutex_);
    sessions_.erase(sessionId);
  }
  return out;
}

json::JsonValue EditSessionManager::snapshots(int64_t photoId, rpc::RpcError* err) {
  std::string sql = "SELECT id, name, created_at FROM snapshots WHERE photo_id=? ORDER BY "
                    "created_at DESC;";
  std::string sqlErr;
  auto stmt = photos_->database()->prepareCached("snapshotList", sql, &sqlErr);
  if (!stmt || !stmt->valid()) {
    *err = rpc::RpcError(rpc::kDbError, sqlErr);
    return json::JsonValue::nullValue();
  }
  stmt->bindInt64(1, photoId);
  json::JsonValue out = json::JsonValue::arrayValue();
  while (stmt->step(&sqlErr)) {
    json::JsonValue item = json::JsonValue::objectValue();
    item.setItem("snapshotId", stmt->columnInt64(0));
    item.setItem("name", stmt->columnText(1));
    item.setItem("createdAt", stmt->columnInt64(2));
    out.addItem(item);
  }
  stmt->reset();
  return out;
}

int64_t EditSessionManager::createSnapshot(int64_t photoId, const std::string& name,
                                           rpc::RpcError* err) {
  json::JsonValue params;
  std::string loadErr;
  if (!xmp_->loadParams(photoId, &params, &loadErr)) {
    *err = rpc::RpcError(rpc::kXmpParseError, loadErr);
    return 0;
  }
  std::string sql =
      "INSERT INTO snapshots(photo_id,name,params_json,created_at) VALUES(?,?,?,?);";
  std::string sqlErr;
  auto stmt = photos_->database()->prepareCached("snapshotCreate", sql, &sqlErr);
  if (!stmt || !stmt->valid()) {
    *err = rpc::RpcError(rpc::kDbError, sqlErr);
    return 0;
  }
  stmt->bindInt64(1, photoId);
  stmt->bindText(2, name);
  stmt->bindText(3, params.serialize());
  stmt->bindInt64(4, nowMs());
  if (!stmt->step(&sqlErr)) {
    *err = rpc::RpcError(rpc::kDbError, sqlErr);
    return 0;
  }
  int64_t id = photos_->database()->lastInsertId();
  stmt->reset();
  return id;
}

json::JsonValue EditSessionManager::restoreSnapshot(int64_t photoId, int64_t snapshotId,
                                                    rpc::RpcError* err) {
  std::string sql = "SELECT params_json FROM snapshots WHERE id=? AND photo_id=?;";
  std::string sqlErr;
  auto stmt = photos_->database()->prepareCached("snapshotGet", sql, &sqlErr);
  if (!stmt || !stmt->valid()) {
    *err = rpc::RpcError(rpc::kDbError, sqlErr);
    return json::JsonValue::nullValue();
  }
  stmt->bindInt64(1, snapshotId);
  stmt->bindInt64(2, photoId);
  if (!stmt->step(&sqlErr)) {
    *err = rpc::RpcError(rpc::kFileNotFound, "snapshot not found");
    return json::JsonValue::nullValue();
  }
  std::string text = stmt->columnText(0);
  stmt->reset();
  json::JsonValue params;
  std::string parseErr;
  if (!json::JsonValue::parse(text, &params, &parseErr)) {
    *err = rpc::RpcError(rpc::kInternalError, "snapshot params corrupt: " + parseErr);
    return json::JsonValue::nullValue();
  }
  std::string saveErr;
  if (!xmp_->saveParams(photoId, params, &saveErr)) {
    *err = rpc::RpcError(rpc::kXmpWriteFailed, saveErr);
    return json::JsonValue::nullValue();
  }
  return params;
}

void EditSessionManager::tick() {
  std::vector<std::shared_ptr<SessionState>> toFlush;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    int64_t now = nowMs();
    for (auto& kv : sessions_) {
      if (kv.second->dirty && now - kv.second->lastChangeMs >= 500) {
        toFlush.push_back(kv.second);
      }
    }
  }
  for (const auto& session : toFlush) {
    std::string err;
    if (flushSession(session, &err)) {
      std::lock_guard<std::mutex> lock(mutex_);
      session->dirty = false;
    } else {
      logWarn("edit", "debounced flush failed for session " +
                          std::to_string(session->sessionId) + ": " + err);
    }
  }
}

void EditSessionManager::shutdown() {
  if (!running_.exchange(false)) return;
  std::vector<std::shared_ptr<SessionState>> all;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& kv : sessions_) all.push_back(kv.second);
  }
  for (const auto& session : all) {
    if (session->dirty) {
      std::string err;
      if (flushSession(session, &err)) session->dirty = false;
      else logError("edit", "shutdown flush failed: " + err);
    }
  }
}

}  // namespace edit
}  // namespace lumina
