// M2 T2.4 · history 落盘 DAO 实现（架构 §3.3.3）
//
// 事务边界（H4 强杀无半写）：append / checkpoint 各自包在 BEGIN IMMEDIATE ... COMMIT
// 里，SQLite WAL 保证进程被杀后要么整体可见要么整体不可见，不存在半写状态。
// ⚠ 性能纪律：语句全部 prepareCached 后复用（单次调用一条语句，非热循环；
//   Database::prepareCached 每次新建语句的成本在此频率下可忽略）。
#include "db/history_dao.h"

#include "core/str_util.h"
#include "db/develop_settings_dao.h"

namespace lumina {
namespace db {

namespace {
// 统一前置守卫：表缺失 → 显式失败（调用方 logWarn，禁止静默）
bool guardAvailable(HistoryDao* dao, const char* op, std::string* err) {
  std::string e;
  if (!dao->available(&e)) {
    if (err) *err = std::string("history 表不可用（") + op + "；迁移 004 未执行？）: " + e;
    return false;
  }
  return true;
}
}  // namespace

std::string HistoryDao::hashOf(const json::JsonValue& params) {
  return DevelopSettingsDao::hashParams(params);
}

bool HistoryDao::available(std::string* err) const {
  auto stmt = db_->prepareCached(
      "histAvail",
      "SELECT name FROM sqlite_master WHERE type='table' AND name='history_entries';", err);
  if (!stmt || !stmt->valid()) return false;
  const bool ok = stmt->step(nullptr);
  stmt->reset();
  return ok;
}

bool HistoryDao::state(int64_t photoId, HistoryStateRow* out, std::string* err) {
  if (!guardAvailable(this, "state", err)) return false;
  auto stmt = db_->prepareCached("histState",
                                 "SELECT current_seq, base_json, base_hash, updated_at "
                                 "FROM history_state WHERE photo_id=?;",
                                 err);
  if (!stmt || !stmt->valid()) return false;
  stmt->bindInt64(1, photoId);
  out->exists = false;
  out->photoId = photoId;
  if (stmt->step(err)) {
    out->exists = true;
    out->currentSeq = stmt->columnInt64(0);
    out->baseJson = stmt->columnText(1);
    out->baseHash = stmt->columnText(2);
    out->updatedAt = stmt->columnInt64(3);
  }
  stmt->reset();
  return true;
}

bool HistoryDao::initBase(int64_t photoId, const json::JsonValue& baseParams, std::string* err) {
  if (!guardAvailable(this, "initBase", err)) return false;
  // 幂等：已有行（无论游标在哪）一律不动——base 只在首次打开时锚定
  auto stmt = db_->prepareCached(
      "histInit",
      "INSERT INTO history_state(photo_id,current_seq,base_json,base_hash,updated_at) "
      "VALUES(?,0,?,?,?) ON CONFLICT(photo_id) DO NOTHING;",
      err);
  if (!stmt || !stmt->valid()) return false;
  stmt->bindInt64(1, photoId);
  stmt->bindText(2, baseParams.serialize());
  stmt->bindText(3, hashOf(baseParams));
  stmt->bindInt64(4, nowMs());
  const bool ok = stmt->step(err);
  stmt->reset();
  return ok;
}

bool HistoryDao::append(int64_t photoId, const json::JsonValue& op, const std::string& label,
                        int64_t sessionId, int64_t* outSeq, std::string* err) {
  if (!guardAvailable(this, "append", err)) return false;
  // 四条语句全部循环外 prepare（本函数单次调用各用一次）
  auto sel = db_->prepareCached(
      "histCurSeq", "SELECT current_seq FROM history_state WHERE photo_id=?;", err);
  auto delRedo = db_->prepareCached(
      "histDelRedo", "DELETE FROM history_entries WHERE photo_id=? AND seq>?;", err);
  auto ins = db_->prepareCached(
      "histAppend",
      "INSERT INTO history_entries(photo_id,seq,op_json,label,created_at,session_id) "
      "VALUES(?,?,?,?,?,?);",
      err);
  auto upd = db_->prepareCached(
      "histAdvSeq", "UPDATE history_state SET current_seq=?, updated_at=? WHERE photo_id=?;", err);
  if (!sel || !sel->valid() || !delRedo || !delRedo->valid() || !ins || !ins->valid() || !upd ||
      !upd->valid()) {
    return false;
  }

  std::string e;
  if (!db_->begin(&e)) {
    if (err) *err = "history append begin 失败: " + e;
    return false;
  }
  // 游标锚点：state 行必须已存在（begin 时 initBase）
  sel->bindInt64(1, photoId);
  int64_t cur = -1;
  if (sel->step(nullptr)) cur = sel->columnInt64(0);
  sel->reset();
  if (cur < 0) {
    db_->rollback(nullptr);
    if (err) *err = "history_state 无行（begin 时应已 initBase）：photo " +
                    std::to_string(photoId);
    return false;
  }
  // 丢弃 redo 分支（与 HistoryStack.push 清 redo 同语义），避免 UNIQUE(photo_id,seq) 撞号
  delRedo->bindInt64(1, photoId);
  delRedo->bindInt64(2, cur);
  delRedo->step(nullptr);
  delRedo->reset();
  const int64_t seq = cur + 1;
  ins->bindInt64(1, photoId);
  ins->bindInt64(2, seq);
  ins->bindText(3, op.serialize());
  ins->bindText(4, label);
  ins->bindInt64(5, nowMs());
  ins->bindText(6, std::to_string(sessionId));
  if (!ins->step(err)) {
    db_->rollback(nullptr);
    ins->reset();
    return false;
  }
  ins->reset();
  upd->bindInt64(1, seq);
  upd->bindInt64(2, nowMs());
  upd->bindInt64(3, photoId);
  if (!upd->step(err)) {
    db_->rollback(nullptr);
    upd->reset();
    return false;
  }
  upd->reset();
  if (!db_->commit(&e)) {
    db_->rollback(nullptr);
    if (err) *err = "history append commit 失败: " + e;
    return false;
  }
  if (outSeq) *outSeq = seq;
  return true;
}

bool HistoryDao::updateSeq(int64_t photoId, int64_t seq, std::string* err) {
  if (!guardAvailable(this, "updateSeq", err)) return false;
  auto stmt = db_->prepareCached(
      "histUpdateSeq",
      "UPDATE history_state SET current_seq=?, updated_at=? WHERE photo_id=?;", err);
  if (!stmt || !stmt->valid()) return false;
  stmt->bindInt64(1, seq);
  stmt->bindInt64(2, nowMs());
  stmt->bindInt64(3, photoId);
  const bool ok = stmt->step(err);
  stmt->reset();
  if (ok && db_->changes() == 0) {
    if (err) *err = "history_state 无行：photo " + std::to_string(photoId);
    return false;
  }
  return ok;
}

bool HistoryDao::checkpoint(int64_t photoId, int64_t baseSeq, const json::JsonValue& baseParams,
                            std::string* err) {
  if (!guardAvailable(this, "checkpoint", err)) return false;
  auto up = db_->prepareCached(
      "histCkpt",
      "INSERT INTO history_state(photo_id,current_seq,base_json,base_hash,updated_at) "
      "VALUES(?,?,?,?,?) ON CONFLICT(photo_id) DO UPDATE SET "
      "current_seq=excluded.current_seq, base_json=excluded.base_json, "
      "base_hash=excluded.base_hash, updated_at=excluded.updated_at;",
      err);
  auto del = db_->prepareCached(
      "histCkptDel", "DELETE FROM history_entries WHERE photo_id=? AND seq<=?;", err);
  if (!up || !up->valid() || !del || !del->valid()) return false;

  std::string e;
  if (!db_->begin(&e)) {
    if (err) *err = "history checkpoint begin 失败: " + e;
    return false;
  }
  up->bindInt64(1, photoId);
  up->bindInt64(2, baseSeq);
  up->bindText(3, baseParams.serialize());
  up->bindText(4, hashOf(baseParams));
  up->bindInt64(5, nowMs());
  if (!up->step(err)) {
    db_->rollback(nullptr);
    up->reset();
    return false;
  }
  up->reset();
  del->bindInt64(1, photoId);
  del->bindInt64(2, baseSeq);
  if (!del->step(err)) {
    db_->rollback(nullptr);
    del->reset();
    return false;
  }
  del->reset();
  if (!db_->commit(&e)) {
    db_->rollback(nullptr);
    if (err) *err = "history checkpoint commit 失败: " + e;
    return false;
  }
  return true;
}

bool HistoryDao::list(int64_t photoId, std::vector<HistoryEntryRow>* out, std::string* err) {
  if (!guardAvailable(this, "list", err)) return false;
  out->clear();
  auto stmt = db_->prepareCached(
      "histList",
      "SELECT seq, op_json, label, created_at FROM history_entries WHERE photo_id=? "
      "ORDER BY seq ASC;",
      err);
  if (!stmt || !stmt->valid()) return false;
  stmt->bindInt64(1, photoId);
  while (stmt->step(err)) {
    HistoryEntryRow row;
    row.seq = stmt->columnInt64(0);
    row.opJson = stmt->columnText(1);
    row.label = stmt->columnText(2);
    row.createdAt = stmt->columnInt64(3);
    out->push_back(row);
  }
  stmt->reset();
  return true;
}

bool HistoryDao::clear(int64_t photoId, std::string* err) {
  if (!guardAvailable(this, "clear", err)) return false;
  std::string e;
  if (!db_->begin(&e)) {
    if (err) *err = "history clear begin 失败: " + e;
    return false;
  }
  if (!db_->exec("DELETE FROM history_entries WHERE photo_id=" + std::to_string(photoId) + ";",
                 &e) ||
      !db_->exec("DELETE FROM history_state WHERE photo_id=" + std::to_string(photoId) + ";", &e)) {
    db_->rollback(nullptr);
    if (err) *err = "history clear 失败: " + e;
    return false;
  }
  if (!db_->commit(&e)) {
    db_->rollback(nullptr);
    if (err) *err = "history clear commit 失败: " + e;
    return false;
  }
  return true;
}

}  // namespace db
}  // namespace lumina
