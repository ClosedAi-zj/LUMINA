// M2 T2.3 · develop_settings 派生缓存实现
//
// 与 sidecar 的关系（唯一真相源在 sidecar，本表随时可删可重建）：
//   读：reconcile 用 sidecar 的 mtime 当一致性锚点，比缓存新就覆盖，坏了就重建；
//   写：只有 upsert（由 EditSessionManager::flushSession 在 sidecar 写成功后调用）；
//   反向：本文件**没有任何**写 sidecar 的代码路径（H2 静态可证）。
#include "db/develop_settings_dao.h"

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

#include "core/logger.h"
#include "core/md5.h"
#include "core/str_util.h"
#include "rpc/version.h"
#include "xmp/xmp_mapper.h"
#include "xmp/xmp_service.h"

namespace lumina {
namespace db {
namespace {

constexpr int kRebuildChunk = 200;   // 重建每批读 200 张，便于取消与进度
constexpr int kBackfillChunk = 50;   // 增量回填批更小：进度更平滑、取消延迟更低

// 表是否存在（迁移 003 是否已跑）。SQL 走 sqlite_master，不猜。
bool tableExists(Database* db, const std::string& table) {
  if (!db || !db->isOpen()) return false;
  auto stmt = db->prepareCached(
      "ds:tableExists",
      "SELECT 1 FROM sqlite_master WHERE type='table' AND name='" + table + "' LIMIT 1;", nullptr);
  if (!stmt || !stmt->valid()) return false;
  const bool found = stmt->step(nullptr);
  stmt->reset();
  return found;
}

void appendEscapedString(const std::string& s, std::string* out) {
  *out += '"';
  for (char c : s) {
    switch (c) {
      case '"': *out += "\\\""; break;
      case '\\': *out += "\\\\"; break;
      case '\n': *out += "\\n"; break;
      case '\r': *out += "\\r"; break;
      case '\t': *out += "\\t"; break;
      default: *out += c; break;
    }
  }
  *out += '"';
}

// 数值统一成 %.9g：0 与 0.0 得到同一串（int/double 噪声不得让 has_edits 误判）
void appendNumber(double v, std::string* out) {
  char buf[48];
  std::snprintf(buf, sizeof(buf), "%.9g", v);
  *out += buf;
}

void canonicalize(const json::JsonValue& v, std::string* out) {
  switch (v.type()) {
    case json::Type::kNull:
      *out += "null";
      break;
    case json::Type::kBool:
      *out += v.asBool() ? "true" : "false";
      break;
    case json::Type::kInt:
      appendNumber(static_cast<double>(v.asInt()), out);
      break;
    case json::Type::kDouble:
      appendNumber(v.asDouble(), out);
      break;
    case json::Type::kString:
      appendEscapedString(v.stringRef(), out);
      break;
    case json::Type::kArray: {
      *out += '[';
      bool first = true;
      for (const auto& item : v.arrayItems()) {
        if (!first) *out += ',';
        first = false;
        canonicalize(item, out);
      }
      *out += ']';
      break;
    }
    case json::Type::kObject: {
      // 键排序：同一批参数无论插入顺序如何都得到同一串（canonical 的核心）
      std::vector<std::pair<std::string, std::string>> items;
      items.reserve(v.objectItems().size());
      for (const auto& kv : v.objectItems()) {
        std::string child;
        canonicalize(kv.second, &child);
        items.emplace_back(kv.first, std::move(child));
      }
      std::sort(items.begin(), items.end(),
                [](const std::pair<std::string, std::string>& a,
                   const std::pair<std::string, std::string>& b) { return a.first < b.first; });
      *out += '{';
      bool first = true;
      for (const auto& kv : items) {
        if (!first) *out += ',';
        first = false;
        appendEscapedString(kv.first, out);
        *out += ':';
        *out += kv.second;
      }
      *out += '}';
      break;
    }
  }
}

// has_edits 比较时忽略的元数据键：引擎每次写 sidecar 都会带上 SchemaVersion，
// 它不是「用户调整」，把它算进去会让每张有 sidecar 的照片都变成「已调整」。
bool isMetadataKey(const std::string& key) { return key == "schemaVersion"; }

}  // namespace

bool DevelopSettingsDao::available(std::string* err) const {
  if (!db_ || !db_->isOpen()) {
    if (err) *err = "develop_settings: database not open";
    return false;
  }
  const bool ok = tableExists(db_, "develop_settings");
  if (!ok && err) *err = "develop_settings 表不存在（迁移 003 未执行）";
  return ok;
}

std::string DevelopSettingsDao::canonicalJson(const json::JsonValue& v) {
  std::string out;
  canonicalize(v, &out);
  return out;
}

std::string DevelopSettingsDao::hashParams(const json::JsonValue& params) {
  const std::string canon = canonicalJson(params);
  return md5OfBuffer(canon.data(), canon.size());
}

int DevelopSettingsDao::countMasks(const json::JsonValue& params) {
  const json::JsonValue& masks = params.get("masks");
  return masks.isArray() ? static_cast<int>(masks.size()) : 0;
}

int DevelopSettingsDao::countNodes(const json::JsonValue& params) {
  const json::JsonValue& nodes = params.get("nodes");
  return nodes.isArray() ? static_cast<int>(nodes.size()) : 0;
}

bool DevelopSettingsDao::computeHasEdits(const json::JsonValue& params) {
  if (!params.isObject()) return false;
  // 与「默认参数」逐键比对：键集合取两侧并集，跳过元数据键。
  // 这样 hsl/colorGrade 这类由扁平键 unflatten 出来的嵌套组也能被判出来。
  const json::JsonValue defaults = xmp::XmpMapper::defaultParams();
  for (const auto& kv : params.objectItems()) {
    if (isMetadataKey(kv.first)) continue;
    if (!defaults.contains(kv.first)) {
      // 默认参数里没有这个键：只要不是空数组/空对象，就说明用户加了东西
      const bool emptyish = (kv.second.isArray() && kv.second.size() == 0) ||
                            (kv.second.isObject() && kv.second.size() == 0) ||
                            kv.second.isNull();
      if (!emptyish) return true;
      continue;
    }
    if (canonicalJson(kv.second) != canonicalJson(defaults.get(kv.first))) return true;
  }
  for (const auto& kv : defaults.objectItems()) {
    if (isMetadataKey(kv.first)) continue;
    if (params.contains(kv.first)) continue;
    // 默认有、参数没有：等价于回到默认值（readFromDoc 一定会补全，这里只是兜底）
    const bool emptyish = (kv.second.isArray() && kv.second.size() == 0) ||
                          (kv.second.isObject() && kv.second.size() == 0) ||
                          kv.second.isNull() ||
                          canonicalJson(kv.second) == "0" || canonicalJson(kv.second) == "false";
    if (!emptyish) return true;
  }
  return false;
}

bool DevelopSettingsDao::upsert(int64_t photoId, const json::JsonValue& params, int64_t sidecarMtime,
                                const std::string& source, std::string* err) {
  if (!db_ || !db_->isOpen()) {
    if (err) *err = "develop_settings: database not open";
    return false;
  }
  if (photoId <= 0) {
    if (err) *err = "develop_settings: photoId 非法";
    return false;
  }
  if (!tableExists(db_, "develop_settings")) {
    if (err) *err = "develop_settings 表不存在（迁移 003 未执行）";
    return false;
  }
  const std::string sql =
      "INSERT INTO develop_settings(photo_id,params_json,params_hash,schema_version,sidecar_mtime,"
      "node_count,mask_count,has_edits,updated_at,source) VALUES(?,?,?,?,?,?,?,?,?,?) "
      "ON CONFLICT(photo_id) DO UPDATE SET params_json=excluded.params_json,"
      "params_hash=excluded.params_hash, schema_version=excluded.schema_version,"
      "sidecar_mtime=excluded.sidecar_mtime, node_count=excluded.node_count,"
      "mask_count=excluded.mask_count, has_edits=excluded.has_edits,"
      "updated_at=excluded.updated_at, source=excluded.source;";
  auto stmt = db_->prepareCached("ds:upsert", sql, err);
  if (!stmt || !stmt->valid()) return false;
  const std::string canon = canonicalJson(params);
  const int schemaVersion = static_cast<int>(params.get("schemaVersion").asInt(kXmpSchemaVersion));
  stmt->bindInt64(1, photoId);
  stmt->bindText(2, canon);
  stmt->bindText(3, md5OfBuffer(canon.data(), canon.size()));
  stmt->bindInt(4, schemaVersion);
  stmt->bindInt64(5, sidecarMtime);
  stmt->bindInt(6, countNodes(params));
  stmt->bindInt(7, countMasks(params));
  stmt->bindInt(8, computeHasEdits(params) ? 1 : 0);
  stmt->bindInt64(9, nowMs() / 1000);
  stmt->bindText(10, source);
  if (!stmt->step(err)) {
    stmt->reset();
    return false;
  }
  stmt->reset();
  return true;
}

bool DevelopSettingsDao::get(int64_t photoId, DevelopSettingsRow* out, bool* found,
                             std::string* err) {
  if (found) *found = false;
  if (out) *out = DevelopSettingsRow();
  if (!db_ || !db_->isOpen()) {
    if (err) *err = "develop_settings: database not open";
    return false;
  }
  if (!tableExists(db_, "develop_settings")) {
    // 降级为「无缓存」，不是错误：调用方按 found=false 走 sidecar 路径
    return true;
  }
  auto stmt = db_->prepareCached(
      "ds:get",
      "SELECT photo_id,params_json,params_hash,schema_version,sidecar_mtime,node_count,mask_count,"
      "has_edits,updated_at,source FROM develop_settings WHERE photo_id=?;",
      err);
  if (!stmt || !stmt->valid()) return false;
  stmt->bindInt64(1, photoId);
  bool ok = true;
  if (stmt->step(err)) {
    if (found) *found = true;
    if (out) {
      out->photoId = stmt->columnInt64(0);
      out->paramsJson = stmt->columnText(1);
      out->paramsHash = stmt->columnText(2);
      out->schemaVersion = stmt->columnInt(3);
      out->sidecarMtime = stmt->columnInt64(4);
      out->nodeCount = stmt->columnInt(5);
      out->maskCount = stmt->columnInt(6);
      out->hasEdits = stmt->columnInt(7) != 0;
      out->updatedAt = stmt->columnInt64(8);
      out->source = stmt->columnText(9);
    }
  } else if (err && !err->empty()) {
    ok = false;
  }
  stmt->reset();
  return ok;
}

bool DevelopSettingsDao::erase(int64_t photoId, std::string* err) {
  if (!db_ || !db_->isOpen()) {
    if (err) *err = "develop_settings: database not open";
    return false;
  }
  if (!tableExists(db_, "develop_settings")) return true;  // 表都没有，删行等价于已达成
  auto stmt = db_->prepareCached("ds:erase", "DELETE FROM develop_settings WHERE photo_id=?;", err);
  if (!stmt || !stmt->valid()) return false;
  stmt->bindInt64(1, photoId);
  const bool ok = stmt->step(err);
  stmt->reset();
  return ok;
}

bool DevelopSettingsDao::markStale(int64_t photoId, std::string* err) {
  if (!db_ || !db_->isOpen()) {
    if (err) *err = "develop_settings: database not open";
    return false;
  }
  if (!tableExists(db_, "develop_settings")) {
    if (err) *err = "develop_settings 表不存在（迁移 003 未执行）";
    return false;
  }
  auto stmt = db_->prepareCached(
      "ds:markStale",
      "UPDATE develop_settings SET source='db-stale', updated_at=? WHERE photo_id=?;", err);
  if (!stmt || !stmt->valid()) return false;
  stmt->bindInt64(1, nowMs() / 1000);
  stmt->bindInt64(2, photoId);
  const bool ok = stmt->step(err);
  stmt->reset();
  return ok;
}

bool DevelopSettingsDao::reconcile(int64_t photoId, const json::JsonValue& sidecarParams,
                                   int64_t sidecarMtime, bool sidecarExists, ReconcileResult* out,
                                   std::string* err) {
  if (out) *out = ReconcileResult();
  if (!db_ || !db_->isOpen()) {
    if (err) *err = "develop_settings: database not open";
    return false;
  }
  if (!tableExists(db_, "develop_settings")) {
    // 迁移 003 未跑：缓存不可用，但**不影响正确性**（参数照旧从 sidecar 读）
    if (out) {
      out->state = "unavailable";
      out->cacheUsable = false;
    }
    return true;
  }

  DevelopSettingsRow row;
  bool found = false;
  if (!get(photoId, &row, &found, err)) return false;

  // ① sidecar 缺失：缓存无对应真相源
  if (!sidecarExists) {
    if (found) {
      if (!markStale(photoId, err)) return false;
      if (out) {
        out->state = "stale";
        out->warning = "参数缓存没有对应的 XMP sidecar（原片可能被外部删除），可「重建 sidecar」";
        out->cacheUsable = true;  // 内容仍可读，但必须提示用户
      }
      logWarn("db", "develop_settings stale (no sidecar) for photo " + std::to_string(photoId));
    } else if (out) {
      out->state = "absent";
    }
    return true;
  }

  // ② 无缓存行：按 sidecar 补建
  if (!found) {
    if (!upsert(photoId, sidecarParams, sidecarMtime, "xmp", err)) return false;
    if (out) {
      out->state = "filled";
      out->cacheUsable = true;
    }
    return true;
  }

  // ③ 缓存行损坏（params_json 解析失败）：删行 + 从 sidecar 重建 + 显式 warning
  json::JsonValue parsed;
  std::string parseErr;
  const bool corrupt = !json::JsonValue::parse(row.paramsJson, &parsed, &parseErr);
  if (corrupt) {
    logWarn("db", "develop_settings 行损坏，已从 sidecar 重建: photo " + std::to_string(photoId) +
                      " (" + parseErr + ")");
    if (!erase(photoId, err)) return false;
    if (!upsert(photoId, sidecarParams, sidecarMtime, "rebuilt", err)) return false;
    if (out) {
      out->state = "repaired";
      out->warning = "参数缓存损坏，已按 XMP sidecar 重建（参数值未变）";
      out->cacheUsable = true;
    }
    return true;
  }

  // ④ sidecar 比缓存新：重读 sidecar 覆盖缓存（debug 日志，不打扰用户）
  if (sidecarMtime > row.sidecarMtime) {
    if (!upsert(photoId, sidecarParams, sidecarMtime, "xmp", err)) return false;
    logDebug("db", "develop_settings refreshed from sidecar: photo " + std::to_string(photoId) +
                       " mtime " + std::to_string(row.sidecarMtime) + " -> " +
                       std::to_string(sidecarMtime));
    if (out) {
      out->state = "refreshed";
      out->cacheUsable = true;
    }
    return true;
  }

  if (out) {
    out->state = "ok";
    out->cacheUsable = true;
  }
  return true;
}

bool DevelopSettingsDao::rebuildFromSidecar(xmp::XmpService* xmp, RebuildReport* rep,
                                            std::atomic<bool>* cancel,
                                            const std::function<void(int, const std::string&)>&
                                                progress,
                                            std::string* err,
                                            const std::function<void()>& yieldBetweenChunks) {
  RebuildReport local;
  RebuildReport& r = rep ? *rep : local;
  if (!db_ || !db_->isOpen()) {
    if (err) *err = "develop_settings: database not open";
    return false;
  }
  if (!xmp) {
    if (err) *err = "develop_settings: rebuildFromSidecar 需要 XmpService（sidecar 是唯一真相源）";
    return false;
  }
  if (!tableExists(db_, "develop_settings")) {
    if (err) *err = "develop_settings 表不存在（迁移 003 未执行）";
    return false;
  }

  int64_t total = 0;
  {
    auto stmt = db_->prepareCached("ds:countPhotos", "SELECT COUNT(*) FROM photos;", err);
    if (!stmt || !stmt->valid()) return false;
    std::string countErr;
    if (stmt->step(&countErr)) {
      total = stmt->columnInt64(0);
    } else if (!countErr.empty()) {
      // 计数失败不能静默按 0 处理：进度分母会变成 0，整次重建看起来「瞬间完成」
      stmt->reset();
      if (err) *err = "develop_settings: 统计照片总数失败: " + countErr;
      return false;
    }
    stmt->reset();
  }
  if (progress) progress(0, "重建参数缓存 0/" + std::to_string(total));

  int64_t lastId = 0;
  while (true) {
    if (cancel && cancel->load()) {
      r.cancelled = true;
      r.detail = "重建被取消（已处理 " + std::to_string(r.scanned) + "/" + std::to_string(total) +
                 "，已提交部分保持有效）";
      logWarn("db", r.detail);
      if (err) *err = "rebuild cancelled";
      return false;
    }
    std::string sql = "SELECT id, file_path, sidecar_path FROM photos WHERE id > " +
                      std::to_string(lastId) + " ORDER BY id LIMIT " +
                      std::to_string(kRebuildChunk) + ";";
    auto stmt = db_->prepareCached("ds:scanPhotos", sql, err);
    if (!stmt || !stmt->valid()) return false;
    struct Row {
      int64_t id = 0;
      std::string filePath;
      std::string sidecarPath;
    };
    std::vector<Row> rows;
    std::string chunkErr;
    while (stmt->step(&chunkErr)) {
      Row row;
      row.id = stmt->columnInt64(0);
      row.filePath = stmt->columnText(1);
      row.sidecarPath = stmt->columnText(2);
      rows.push_back(row);
    }
    stmt->reset();
    // 批次枚举失败必须终止：若当成「本批读完了」就会把错判成缩容，
    // 后面 rows.empty() 直接 break 并回报成功（静默截断重建）。
    if (!chunkErr.empty()) {
      if (err) *err = "develop_settings: 扫描照片批次失败: " + chunkErr;
      return false;
    }
    if (rows.empty()) break;
    lastId = rows.back().id;

    for (const Row& row : rows) {
      ++r.scanned;
      const std::string sidecar =
          row.sidecarPath.empty() ? xmp->sidecarPathFor(row.filePath) : row.sidecarPath;
      const bool exists = fileExists(sidecar);
      if (!exists) {
        // 派生缓存无真相源：删行（保留会让「含蒙版/已调整」筛选出现幽灵命中）。
        // 注意：从未有过缓存行的照片（未调整）不该计入 removed —— 用 changes() 区分。
        if (!erase(row.id, err)) return false;
        if (db_->changes() > 0) {
          ++r.removed;
          logWarn("db",
                  "rebuild: photo " + std::to_string(row.id) + " 无 sidecar，已清除缓存行");
        }
        continue;
      }
      json::JsonValue params;
      std::string loadErr;
      if (!xmp->loadParams(row.id, &params, &loadErr)) {
        // 解析失败：删行 + 计数 + 显式留痕（不静默跳过）
        if (!erase(row.id, err)) return false;
        if (db_->changes() > 0) ++r.removed;
        ++r.failed;
        logWarn("db", "rebuild: photo " + std::to_string(row.id) + " sidecar 解析失败，已删缓存行: " +
                          loadErr);
        continue;
      }
      if (!upsert(row.id, params, fileMtimeMs(sidecar), "rebuilt", err)) {
        ++r.failed;
        logError("db", "rebuild: upsert failed for photo " + std::to_string(row.id));
        continue;
      }
      ++r.written;
    }
    if (progress && total > 0) {
      const int pct = static_cast<int>(std::min<int64_t>(100, r.scanned * 100 / total));
      progress(pct, "重建参数缓存 " + std::to_string(r.scanned) + "/" + std::to_string(total));
    }
    if (yieldBetweenChunks) yieldBetweenChunks();
  }
  r.detail = "重建完成：扫描 " + std::to_string(r.scanned) + "，写入 " + std::to_string(r.written) +
             "，清除 " + std::to_string(r.removed) + "，失败 " + std::to_string(r.failed);
  if (progress) progress(100, r.detail);
  logInfo("db", r.detail);
  return true;
}

bool DevelopSettingsDao::cachedRowCount(int64_t* out, std::string* err) {
  if (out) *out = 0;
  if (!db_ || !db_->isOpen()) {
    if (err) *err = "develop_settings: database not open";
    return false;
  }
  if (!tableExists(db_, "develop_settings")) return true;  // 表不存在 = 0 行（可用性走 available()）
  auto stmt = db_->prepareCached("ds:rowCount", "SELECT COUNT(*) FROM develop_settings;", err);
  if (!stmt || !stmt->valid()) return false;
  // 零静默：step 失败（库损坏/锁死）必须报错，不得返回「0 行」让调用方误判成空表
  // 而走全量重建——job 侧收到 false 会以 mode=unavailable 显式留痕。
  if (!stmt->step(err)) {
    stmt->reset();
    if (err && err->empty()) *err = "develop_settings: cachedRowCount 查询失败";
    return false;
  }
  if (out) *out = stmt->columnInt64(0);
  stmt->reset();
  return true;
}

bool DevelopSettingsDao::backfillFromSidecar(xmp::XmpService* xmp, RebuildReport* rep,
                                             std::atomic<bool>* cancel,
                                             const std::function<void(int, const std::string&)>&
                                                 progress,
                                             std::string* err,
                                             const std::function<void()>& yieldBetweenChunks) {
  RebuildReport local;
  RebuildReport& r = rep ? *rep : local;
  if (!db_ || !db_->isOpen()) {
    if (err) *err = "develop_settings: database not open";
    return false;
  }
  if (!xmp) {
    if (err) *err = "develop_settings: backfillFromSidecar 需要 XmpService（sidecar 是唯一真相源）";
    return false;
  }
  if (!tableExists(db_, "develop_settings")) {
    if (err) *err = "develop_settings 表不存在（迁移 003 未执行）";
    return false;
  }

  int64_t total = 0;
  {
    auto stmt = db_->prepareCached("ds:backfillCountPhotos", "SELECT COUNT(*) FROM photos;", err);
    if (!stmt || !stmt->valid()) return false;
    if (stmt->step(nullptr)) total = stmt->columnInt64(0);
    stmt->reset();
  }
  if (progress) progress(0, "增量回填 0/" + std::to_string(total));

  int64_t lastId = 0;
  int64_t judged = 0;  // 已判定（含跳过）的照片数，进度用它而不是 r.scanned（候选数会跳变）
  while (true) {
    if (cancel && cancel->load()) {
      r.cancelled = true;
      r.detail = "增量回填被取消（已判定 " + std::to_string(judged) + "/" +
                 std::to_string(total) + "，已提交部分保持有效）";
      logWarn("db", r.detail);
      if (err) *err = "backfill cancelled";
      return false;
    }
    // LEFT JOIN 一次取回「缓存行是否存在 + 缓存 mtime」；无缓存行的照片
    // ds.sidecar_mtime 为 NULL，用 columnIsNull 区分（不是错误）。
    std::string sql =
        "SELECT p.id, p.file_path, p.sidecar_path, ds.sidecar_mtime "
        "FROM photos p LEFT JOIN develop_settings ds ON ds.photo_id=p.id "
        "WHERE p.id > " + std::to_string(lastId) + " ORDER BY p.id LIMIT " +
        std::to_string(kBackfillChunk) + ";";
    auto stmt = db_->prepareCached("ds:backfillScan", sql, err);
    if (!stmt || !stmt->valid()) return false;
    struct Row {
      int64_t id = 0;
      std::string filePath;
      std::string sidecarPath;
      bool hasCache = false;
      int64_t cacheMtime = 0;
    };
    std::vector<Row> rows;
    while (stmt->step(nullptr)) {
      Row row;
      row.id = stmt->columnInt64(0);
      row.filePath = stmt->columnText(1);
      row.sidecarPath = stmt->columnText(2);
      row.hasCache = !stmt->columnIsNull(3);
      row.cacheMtime = row.hasCache ? stmt->columnInt64(3) : 0;
      rows.push_back(row);
    }
    stmt->reset();
    if (rows.empty()) break;
    lastId = rows.back().id;

    for (const Row& row : rows) {
      ++judged;
      const std::string sidecar =
          row.sidecarPath.empty() ? xmp->sidecarPathFor(row.filePath) : row.sidecarPath;
      bool candidate = false;
      int64_t diskMtime = 0;
      if (!row.hasCache) {
        // 无缓存行：盘上有 sidecar 才是候选（未调整照片本就不该有缓存行）
        if (fileExists(sidecar)) {
          candidate = true;
          diskMtime = fileMtimeMs(sidecar);
        }
      } else if (!fileExists(sidecar)) {
        // 缓存行还在但 sidecar 丢失：与全量重建同口径删行，防「已调整/含蒙版」筛选幽灵命中
        if (!erase(row.id, err)) return false;
        if (db_->changes() > 0) {
          ++r.removed;
          logWarn("db",
                  "backfill: photo " + std::to_string(row.id) + " 无 sidecar，已清除缓存行");
        }
        continue;
      } else {
        diskMtime = fileMtimeMs(sidecar);
        // 缓存 mtime 落后盘上 mtime 即重读（不限定 has_edits，见头文件口径注释）
        if (diskMtime > row.cacheMtime) candidate = true;
      }
      if (!candidate) continue;
      ++r.scanned;
      json::JsonValue params;
      std::string loadErr;
      if (!xmp->loadParams(row.id, &params, &loadErr)) {
        ++r.failed;
        logWarn("db", "backfill: photo " + std::to_string(row.id) +
                          " sidecar 解析失败，跳过该行: " + loadErr);
        continue;
      }
      if (!upsert(row.id, params, diskMtime, "rebuilt", err)) {
        ++r.failed;
        logError("db", "backfill: upsert failed for photo " + std::to_string(row.id));
        continue;
      }
      ++r.written;
    }
    if (progress && total > 0) {
      const int pct = static_cast<int>(std::min<int64_t>(100, judged * 100 / total));
      progress(pct, "增量回填 " + std::to_string(judged) + "/" + std::to_string(total) +
                        "，更新 " + std::to_string(r.written));
    }
    if (yieldBetweenChunks) yieldBetweenChunks();
  }
  r.detail = "增量回填完成：判定 " + std::to_string(judged) + "，回填 " +
             std::to_string(r.scanned) + "，写入 " + std::to_string(r.written) + "，清除 " +
             std::to_string(r.removed) + "，失败 " + std::to_string(r.failed);
  if (progress) progress(100, r.detail);
  logInfo("db", r.detail);
  return true;
}

bool DevelopSettingsDao::queryEdited(std::vector<int64_t>* out, int limit, std::string* err) {
  if (!out) {
    if (err) *err = "develop_settings: out 为空";
    return false;
  }
  out->clear();
  if (!db_ || !db_->isOpen()) {
    if (err) *err = "develop_settings: database not open";
    return false;
  }
  if (!tableExists(db_, "develop_settings")) {
    if (err) *err = "develop_settings 表不存在（迁移 003 未执行）";
    return false;
  }
  // 条件与 idx_dev_edited 的 WHERE 子句一致，才能命中部分索引
  auto stmt = db_->prepareCached(
      "ds:queryEdited",
      "SELECT photo_id FROM develop_settings WHERE has_edits=1 ORDER BY photo_id LIMIT ?;", err);
  if (!stmt || !stmt->valid()) return false;
  stmt->bindInt(1, limit <= 0 ? 1000 : limit);
  while (stmt->step(nullptr)) out->push_back(stmt->columnInt64(0));
  stmt->reset();
  return true;
}

bool DevelopSettingsDao::queryWithMasks(std::vector<int64_t>* out, int limit, std::string* err) {
  if (!out) {
    if (err) *err = "develop_settings: out 为空";
    return false;
  }
  out->clear();
  if (!db_ || !db_->isOpen()) {
    if (err) *err = "develop_settings: database not open";
    return false;
  }
  if (!tableExists(db_, "develop_settings")) {
    if (err) *err = "develop_settings 表不存在（迁移 003 未执行）";
    return false;
  }
  auto stmt = db_->prepareCached(
      "ds:queryMasks",
      "SELECT photo_id FROM develop_settings WHERE mask_count>0 ORDER BY photo_id LIMIT ?;", err);
  if (!stmt || !stmt->valid()) return false;
  stmt->bindInt(1, limit <= 0 ? 1000 : limit);
  while (stmt->step(nullptr)) out->push_back(stmt->columnInt64(0));
  stmt->reset();
  return true;
}

}  // namespace db
}  // namespace lumina
