// M2 T2.3 · develop_settings 派生缓存自测（H5 可重建 0 差异 / H6 损坏自愈 / 筛选走索引）
//
// 本测试的核心命题只有一句：**develop_settings 是可丢弃派生，sidecar 才是真相源。**
// 因此每条断言都在问「如果缓存消失/损坏，用户看到的参数会变吗？」——答案是必须不变。
//
// 覆盖：
//   M1  迁移 003 幂等（二次启动 0 变更）+ 删表后可由版本化迁移重建
//   M2  纯函数：canonicalJson 键序无关、computeHasEdits 不被 schemaVersion 干扰
//   H5  50 张有调整照片：rebuildFromSidecar 后逐条相等（0 差异）；删表重建后同样 0 差异
//   H6  params_json 损坏 → 删行 + 从 sidecar 重建 + 显式 warning，且参数值不变
//   C   一致性巡检五个状态：absent / filled / ok / refreshed / stale
//   F   edited / hasMasks 筛选正确且构成严格划分；EXPLAIN QUERY PLAN 证明走部分索引
//   D   删表后参数读取结果不变（证明缓存不是真相源）
#include <atomic>
#include <cstdio>
#include <string>
#include <vector>

#include "core/config.h"
#include "core/str_util.h"
#include "core/thread_pool.h"
#include "db/database.h"
#include "db/develop_settings_dao.h"
#include "db/migrations.h"
#include "db/photo_dao.h"
#include "json/json_value.h"
#include "xmp/xmp_mapper.h"
#include "xmp/xmp_service.h"

using namespace lumina;

namespace {

int gFailures = 0;

void check(bool condition, const std::string& name) {
  if (condition) {
    printf("  [PASS] %s\n", name.c_str());
  } else {
    printf("  [FAIL] %s\n", name.c_str());
    ++gFailures;
  }
}

void info(const std::string& text) { printf("  [INFO] %s\n", text.c_str()); }

const char* kHome = "D:/LUMINA/build/runtime/test-dev-settings/home";
const char* kPhotoDir = "D:/LUMINA/build/runtime/test-dev-settings/photos";

// 行快照：H5 只比「数据列」，不比记账列（source / updated_at 每次重建都会变）
struct RowSnapshot {
  int64_t photoId = 0;
  std::string paramsJson;
  std::string paramsHash;
  int schemaVersion = 0;
  int64_t sidecarMtime = 0;
  int nodeCount = 0;
  int maskCount = 0;
  bool hasEdits = false;
};

RowSnapshot snapshotOf(const db::DevelopSettingsRow& r) {
  RowSnapshot s;
  s.photoId = r.photoId;
  s.paramsJson = r.paramsJson;
  s.paramsHash = r.paramsHash;
  s.schemaVersion = r.schemaVersion;
  s.sidecarMtime = r.sidecarMtime;
  s.nodeCount = r.nodeCount;
  s.maskCount = r.maskCount;
  s.hasEdits = r.hasEdits;
  return s;
}

bool sameSnapshot(const RowSnapshot& a, const RowSnapshot& b) {
  return a.photoId == b.photoId && a.paramsJson == b.paramsJson && a.paramsHash == b.paramsHash &&
         a.schemaVersion == b.schemaVersion && a.sidecarMtime == b.sidecarMtime &&
         a.nodeCount == b.nodeCount && a.maskCount == b.maskCount && a.hasEdits == b.hasEdits;
}

// 抓一行快照（不存在返回 false）
bool snapshotRow(db::DevelopSettingsDao* dao, int64_t photoId, RowSnapshot* out) {
  db::DevelopSettingsRow row;
  bool found = false;
  std::string err;
  if (!dao->get(photoId, &row, &found, &err)) return false;
  if (!found) return false;
  *out = snapshotOf(row);
  return true;
}

// EXPLAIN QUERY PLAN 的 detail 串拼接（用于断言是否命中指定索引）
std::string queryPlan(db::Database* db, const std::string& sql, std::string* err) {
  auto stmt = db->prepareCached("explain:" + sql.substr(0, 24), "EXPLAIN QUERY PLAN " + sql, err);
  if (!stmt || !stmt->valid()) return std::string();
  std::string plan;
  while (stmt->step(nullptr)) {
    if (!plan.empty()) plan += " | ";
    plan += stmt->columnText(3);  // EXPLAIN QUERY PLAN 的列：id,parent,notused,detail
  }
  stmt->reset();
  return plan;
}

}  // namespace

int main() {
  printf("=== develop_settings 派生缓存自测（H5/H6）===\n");

  makeDirs(kPhotoDir);
  makeDirs(kHome);
  // 清理上次残留（config.dbPath() = <home>/lumina.db，不是 <home>/db/lumina.db）
  removeFile(std::string(kHome) + "/lumina.db");
  removeFile(std::string(kHome) + "/lumina.db-wal");
  removeFile(std::string(kHome) + "/lumina.db-shm");

  std::string err;
  core::Config config;
  if (!config.init(kHome, &err)) {
    printf("  [FAIL] config.init: %s\n", err.c_str());
    return 1;
  }
  config.load(&err);

  db::Database database;
  if (!database.open(config.dbPath(), &err) || !database.runMigrations(&err)) {
    printf("  [FAIL] database open/migrate: %s\n", err.c_str());
    return 1;
  }
  db::PhotoDao photos(&database);
  db::DevelopSettingsDao dao(&database);
  core::ThreadPool pool(2);
  xmp::XmpService xmp(&config, &photos, &database, &pool);

  // ---------------- M1 迁移 003 幂等 ----------------
  printf("\n[M1] 迁移 003 幂等与版本推进\n");
  check(database.schemaVersion() == db::kTargetDbVersion,
        "db_version = kTargetDbVersion = " + std::to_string(db::kTargetDbVersion));
  {
    db::MigrationReport rep;
    int code = db::kMigrationOk;
    check(db::runVersionedMigrations(&database, &code, &rep), "二次 runVersionedMigrations 成功");
    check(rep.upToDate && rep.statementsRun == 0,
          "二次迁移 0 变更（upToDate=" + std::string(rep.upToDate ? "true" : "false") +
              ", statements=" + std::to_string(rep.statementsRun) + "）");
    std::string availErr;
    check(dao.available(&availErr), "develop_settings 表存在（" + availErr + "）");
  }

  // ---------------- M2 纯函数 ----------------
  printf("\n[M2] 纯函数：canonicalJson / computeHasEdits / hashParams\n");
  {
    json::JsonValue a = json::JsonValue::objectValue();
    a.setItem("exposure", 0.0);
    a.setItem("masks", json::JsonValue::arrayValue());
    json::JsonValue b = json::JsonValue::objectValue();
    b.setItem("masks", json::JsonValue::arrayValue());
    b.setItem("exposure", 0);  // int 0 与 double 0.0 必须得到同一串
    check(db::DevelopSettingsDao::canonicalJson(a) == db::DevelopSettingsDao::canonicalJson(b),
          "canonicalJson 与键插入顺序、int/double 写法无关");
    check(db::DevelopSettingsDao::hashParams(a) == db::DevelopSettingsDao::hashParams(b),
          "hashParams 对等价参数同值");

    json::JsonValue def = xmp::XmpMapper::defaultParams();
    check(!db::DevelopSettingsDao::computeHasEdits(def), "默认参数 → has_edits=0");
    json::JsonValue onlyMeta = def;
    onlyMeta.setItem("schemaVersion", static_cast<int64_t>(2));
    check(!db::DevelopSettingsDao::computeHasEdits(onlyMeta),
          "只有 schemaVersion 差异 → 仍判未调整（元数据键不参与判据）");
    json::JsonValue edited = def;
    edited.setItem("exposure", 0.5);
    check(db::DevelopSettingsDao::computeHasEdits(edited), "exposure=0.5 → has_edits=1");
    json::JsonValue masked = def;
    json::JsonValue masks = json::JsonValue::arrayValue();
    json::JsonValue m = json::JsonValue::objectValue();
    m.setItem("type", "radial");
    masks.addItem(m);
    masked.setItem("masks", masks);
    check(db::DevelopSettingsDao::computeHasEdits(masked), "有蒙版 → has_edits=1");
    check(db::DevelopSettingsDao::countMasks(masked) == 1, "countMasks = 1");
    check(db::DevelopSettingsDao::countMasks(def) == 0, "countMasks(默认) = 0");
  }

  // ---------------- 造 50 张有调整的照片（前 10 张含蒙版）----------------
  printf("\n[夹具] 50 张有调整照片（前 10 张含蒙版）+ 5 张未调整\n");
  const int kEdited = 50;
  const int kMasked = 10;
  const int kPlain = 5;
  const int64_t rootId = photos.ensureRoot(kPhotoDir, "test", &err);
  const int64_t folderId = photos.ensureFolder(rootId, "2026", "2026", &err);
  check(rootId > 0 && folderId > 0, "root/folder 就绪");

  std::vector<int64_t> editedIds;
  std::vector<int64_t> plainIds;
  for (int i = 0; i < kEdited + kPlain; ++i) {
    PhotoRecord p;
    p.fileName = "DS_" + std::to_string(1000 + i) + ".RW2";
    p.filePath = std::string(kPhotoDir) + "/" + p.fileName;
    p.format = "RW2";
    p.rootId = rootId;
    p.folderId = folderId;
    p.capturedAt = 1767225600000LL + i * 1000;
    p.importedAt = nowMs();
    p.width = 64;
    p.height = 48;
    p.health = "ok";
    int64_t id = 0;
    if (!photos.upsert(p, &id, &err)) {
      printf("  [FAIL] upsert %s: %s\n", p.fileName.c_str(), err.c_str());
      ++gFailures;
      continue;
    }
    if (i < kEdited) {
      editedIds.push_back(id);
      json::JsonValue params = xmp::XmpMapper::defaultParams();
      params.setItem("exposure", 0.1 * static_cast<double>(i % 7 + 1));
      params.setItem("contrast", static_cast<double>(i % 11));
      if (i < kMasked) {
        json::JsonValue masks = json::JsonValue::arrayValue();
        json::JsonValue m = json::JsonValue::objectValue();
        m.setItem("id", "mask-" + std::to_string(i));
        m.setItem("type", "radial");
        masks.addItem(m);
        params.setItem("masks", masks);
      }
      // 走真实写入路径（XmpService::saveParams 是唯一参数写入口）
      std::string saveErr;
      if (!xmp.saveParams(id, params, &saveErr, /*force=*/true)) {
        printf("  [FAIL] saveParams %s: %s\n", p.fileName.c_str(), saveErr.c_str());
        ++gFailures;
      }
    } else {
      plainIds.push_back(id);
    }
  }
  check(static_cast<int>(editedIds.size()) == kEdited, "50 张已调整照片就绪");
  check(static_cast<int>(plainIds.size()) == kPlain, "5 张未调整照片就绪");

  // 缓存行由 flushSession 在真实运行中写入；测试直接按同一写序模拟：
  // 「sidecar 已存在 → 再 upsert 缓存」，mtime 取盘上真实值。
  for (int64_t id : editedIds) {
    json::JsonValue params;
    std::string loadErr;
    PhotoRecord rec;
    std::string recErr;
    if (!xmp.loadParams(id, &params, &loadErr) || !photos.getById(id, &rec, &recErr)) {
      printf("  [FAIL] loadParams %lld: %s\n", static_cast<long long>(id), loadErr.c_str());
      ++gFailures;
      continue;
    }
    const std::string sidecar =
        rec.sidecarPath.empty() ? xmp.sidecarPathFor(rec.filePath) : rec.sidecarPath;
    std::string upErr;
    if (!dao.upsert(id, params, fileMtimeMs(sidecar), "xmp", &upErr)) {
      printf("  [FAIL] upsert %lld: %s\n", static_cast<long long>(id), upErr.c_str());
      ++gFailures;
    }
  }

  // ---------------- H5-a 重建后逐条相等 ----------------
  printf("\n[H5-a] rebuildFromSidecar 后逐条相等（0 差异）\n");
  std::vector<RowSnapshot> before;
  before.reserve(editedIds.size());
  for (int64_t id : editedIds) {
    RowSnapshot s;
    if (snapshotRow(&dao, id, &s)) before.push_back(s);
  }
  check(before.size() == editedIds.size(), "重建前 " + std::to_string(before.size()) + " 行快照");

  {
    db::RebuildReport rep;
    std::atomic<bool> cancel(false);
    std::string rbErr;
    const bool ok = dao.rebuildFromSidecar(&xmp, &rep, &cancel, nullptr, &rbErr);
    check(ok, "rebuildFromSidecar 成功（" + rbErr + "）");
    check(rep.written == kEdited, "写入 " + std::to_string(rep.written) + " 行（期望 " +
                                      std::to_string(kEdited) + "）");
    check(rep.failed == 0, "失败 0 行（实际 " + std::to_string(rep.failed) + "）");
    check(rep.removed == 0, "未调整照片无缓存行，未被误删（removed=" +
                                std::to_string(rep.removed) + "）");
  }
  int diffs = 0;
  for (const RowSnapshot& s : before) {
    RowSnapshot after;
    if (!snapshotRow(&dao, s.photoId, &after)) {
      ++diffs;
      printf("  [FAIL] photo %lld 重建后行丢失\n", static_cast<long long>(s.photoId));
      continue;
    }
    if (!sameSnapshot(s, after)) {
      ++diffs;
      printf("  [FAIL] photo %lld 重建后不一致\n", static_cast<long long>(s.photoId));
      if (s.paramsJson != after.paramsJson) printf("         params_json 变了\n");
      if (s.paramsHash != after.paramsHash) printf("         params_hash 变了\n");
      if (s.maskCount != after.maskCount) printf("         mask_count 变了\n");
      if (s.hasEdits != after.hasEdits) printf("         has_edits 变了\n");
      if (s.sidecarMtime != after.sidecarMtime) printf("         sidecar_mtime 变了\n");
    }
  }
  check(diffs == 0, "H5-a：50 张逐条相等，差异 " + std::to_string(diffs) + " 条");

  // ---------------- H5-b 删表重建后同样 0 差异 ----------------
  printf("\n[H5-b] 删 develop_settings 表 → 重新迁移 → 重建后同样 0 差异\n");
  {
    std::string dropErr;
    check(database.exec("DROP TABLE develop_settings;", &dropErr),
          "删表成功（" + dropErr + "）");
    std::string availErr;
    check(!dao.available(&availErr), "删表后 available() = false（" + availErr + "）");

    // 版本回退到 2 再迁移：走真实版本化路径重建 003
    check(db::writeDbVersion(&database, db::kDbVersionFilesTable, nullptr), "db_version 回退到 2");
    db::MigrationReport rep;
    int code = db::kMigrationOk;
    check(db::runVersionedMigrations(&database, &code, &rep), "重新迁移成功");
    check(rep.devTableCreated == 1, "003 重建了 develop_settings 表");
    check(rep.filesInserted == 0 && rep.photosLinked == 0, "002 未重复执行（files+0 / linked+0）");
    check(db::readDbVersion(&database) == db::kTargetDbVersion, "db_version 回到 " +
                                                                   std::to_string(db::kTargetDbVersion));

    db::RebuildReport rb;
    std::atomic<bool> cancel(false);
    std::string rbErr;
    check(dao.rebuildFromSidecar(&xmp, &rb, &cancel, nullptr, &rbErr),
          "删表后重建成功（" + rbErr + "）");
    int diffs2 = 0;
    for (const RowSnapshot& s : before) {
      RowSnapshot after;
      if (!snapshotRow(&dao, s.photoId, &after) || !sameSnapshot(s, after)) ++diffs2;
    }
    check(diffs2 == 0, "H5-b：删表重建后逐条相等，差异 " + std::to_string(diffs2) + " 条");
  }

  // ---------------- H6 损坏自愈 ----------------
  printf("\n[H6] params_json 损坏 → 删行 + 从 sidecar 重建 + 显式 warning\n");
  {
    const int64_t victim = editedIds[0];
    json::JsonValue sidecarParams;
    std::string loadErr;
    check(xmp.loadParams(victim, &sidecarParams, &loadErr), "读回 victim 的 sidecar 参数");

    std::string sqlErr;
    check(database.exec("UPDATE develop_settings SET params_json='{broken json' WHERE photo_id=" +
                            std::to_string(victim) + ";",
                        &sqlErr),
          "把 victim 的 params_json 改坏");

    PhotoRecord rec;
    std::string recErr;
    photos.getById(victim, &rec, &recErr);
    const std::string sidecar =
        rec.sidecarPath.empty() ? xmp.sidecarPathFor(rec.filePath) : rec.sidecarPath;

    db::ReconcileResult rr;
    std::string reErr;
    check(dao.reconcile(victim, sidecarParams, fileMtimeMs(sidecar), true, &rr, &reErr),
          "reconcile 成功（" + reErr + "）");
    check(rr.state == "repaired", "状态 = repaired（实际 " + rr.state + "）");
    check(!rr.warning.empty(), "warning 非空（零静默）：" + rr.warning);

    db::DevelopSettingsRow row;
    bool found = false;
    check(dao.get(victim, &row, &found, &reErr) && found, "victim 行已重建");
    json::JsonValue repaired;
    std::string parseErr;
    check(json::JsonValue::parse(row.paramsJson, &repaired, &parseErr),
          "重建后的 params_json 可解析（" + parseErr + "）");
    check(db::DevelopSettingsDao::canonicalJson(repaired) ==
              db::DevelopSettingsDao::canonicalJson(sidecarParams),
          "H6：参数值不变（与 sidecar 逐字相等）");
    check(row.paramsHash == db::DevelopSettingsDao::hashParams(sidecarParams), "params_hash 同步修正");
  }

  // ---------------- C 一致性巡检五个状态 ----------------
  printf("\n[C] 一致性巡检状态机\n");
  {
    // absent：既无 sidecar 也无缓存行
    const int64_t plain = plainIds[0];
    json::JsonValue def = xmp::XmpMapper::defaultParams();
    db::ReconcileResult rr;
    std::string reErr;
    check(dao.reconcile(plain, def, 0, false, &rr, &reErr), "absent: reconcile 成功");
    check(rr.state == "absent", "absent: 状态 = " + rr.state);

    // filled：有 sidecar 但无缓存行
    const int64_t target = editedIds[1];
    json::JsonValue params;
    std::string loadErr;
    xmp.loadParams(target, &params, &loadErr);
    PhotoRecord rec;
    std::string recErr;
    photos.getById(target, &rec, &recErr);
    const std::string sidecar =
        rec.sidecarPath.empty() ? xmp.sidecarPathFor(rec.filePath) : rec.sidecarPath;
    const int64_t mtime = fileMtimeMs(sidecar);
    dao.erase(target, nullptr);
    check(dao.reconcile(target, params, mtime, true, &rr, &reErr), "filled: reconcile 成功");
    check(rr.state == "filled", "filled: 状态 = " + rr.state);

    // ok：再跑一次
    check(dao.reconcile(target, params, mtime, true, &rr, &reErr), "ok: reconcile 成功");
    check(rr.state == "ok", "ok: 状态 = " + rr.state);

    // refreshed：缓存记的 mtime 比盘上旧
    dao.upsert(target, params, mtime - 5000, "xmp", nullptr);
    check(dao.reconcile(target, params, mtime, true, &rr, &reErr), "refreshed: reconcile 成功");
    check(rr.state == "refreshed", "refreshed: 状态 = " + rr.state);

    // stale：sidecar 被外部删除
    removeFile(sidecar);
    check(dao.reconcile(target, params, 0, false, &rr, &reErr), "stale: reconcile 成功");
    check(rr.state == "stale", "stale: 状态 = " + rr.state);
    check(!rr.warning.empty(), "stale: warning 非空 → " + rr.warning);
    db::DevelopSettingsRow staleRow;
    bool found = false;
    dao.get(target, &staleRow, &found, nullptr);
    check(found && staleRow.source == "db-stale", "stale: source='db-stale'（实际 " +
                                                     (found ? staleRow.source : std::string("无行")) +
                                                     "）");
    // 复原 sidecar，后续用例不受影响
    std::string saveErr;
    xmp.saveParams(target, params, &saveErr, /*force=*/true);
  }

  // ---------------- F 筛选 + 索引 ----------------
  printf("\n[F] edited / hasMasks 筛选与索引命中\n");
  {
    Filter f;
    f.edited = true;
    const int64_t editedCount = photos.count(f, &err);
    check(editedCount == kEdited, "edited=true 命中 " + std::to_string(editedCount) + " 张（期望 " +
                                      std::to_string(kEdited) + "）");

    Filter notEdited;
    notEdited.edited = false;
    const int64_t notEditedCount = photos.count(notEdited, &err);
    check(editedCount + notEditedCount == kEdited + kPlain,
          "edited=true + edited=false = 全集（" + std::to_string(editedCount) + " + " +
              std::to_string(notEditedCount) + " = " + std::to_string(kEdited + kPlain) + "）");

    Filter masked;
    masked.hasMasks = true;
    const int64_t maskedCount = photos.count(masked, &err);
    check(maskedCount == kMasked, "hasMasks=true 命中 " + std::to_string(maskedCount) + " 张（期望 " +
                                      std::to_string(kMasked) + "）");

    Filter noMask;
    noMask.hasMasks = false;
    const int64_t noMaskCount = photos.count(noMask, &err);
    check(noMaskCount == kEdited - kMasked + kPlain,
          "hasMasks=false 命中 " + std::to_string(noMaskCount) + " 张（期望 " +
              std::to_string(kEdited - kMasked + kPlain) + "）");

    std::vector<int64_t> ids;
    const bool qeOk = dao.queryEdited(&ids, 1000, &err);
    check(qeOk && static_cast<int>(ids.size()) == kEdited,
          "queryEdited 走 DAO：" + std::to_string(ids.size()) + " 行（期望 " +
              std::to_string(kEdited) + "）");
    const bool qmOk = dao.queryWithMasks(&ids, 1000, &err);
    check(qmOk && static_cast<int>(ids.size()) == kMasked,
          "queryWithMasks 走 DAO：" + std::to_string(ids.size()) + " 行（期望 " +
              std::to_string(kMasked) + "）");

    // 索引命中：条件必须与部分索引的 WHERE 子句同形，否则规划器用不上
    const std::string planEdited =
        queryPlan(&database, "SELECT photo_id FROM develop_settings WHERE has_edits=1;", &err);
    info("PLAN edited: " + planEdited);
    check(planEdited.find("idx_dev_edited") != std::string::npos,
          "edited 筛选命中 idx_dev_edited");

    const std::string planMasks =
        queryPlan(&database, "SELECT photo_id FROM develop_settings WHERE mask_count>0;", &err);
    info("PLAN masks : " + planMasks);
    check(planMasks.find("idx_dev_masks") != std::string::npos, "hasMasks 筛选命中 idx_dev_masks");

    const std::string planJoin = queryPlan(
        &database,
        "SELECT COUNT(*) FROM photos p WHERE p.deleted=0 AND EXISTS(SELECT 1 FROM develop_settings "
        "dm WHERE dm.photo_id=p.id AND dm.mask_count>0);",
        &err);
    info("PLAN join  : " + planJoin);
    // 关联子查询按 photo_id 取行时，photo_id 是 INTEGER PRIMARY KEY（rowid 别名），
    // 规划器走主键比走部分索引更快——两者都是索引查找，关键是不能全表扫 dm。
    check(planJoin.find("SCAN dm") == std::string::npos,
          "关联子查询未全表扫 develop_settings（走主键或部分索引）");
    check(planJoin.find("INTEGER PRIMARY KEY") != std::string::npos ||
              planJoin.find("idx_dev_masks") != std::string::npos,
          "关联子查询按 photo_id 做索引查找");
  }

  // ---------------- D 删表后参数读取不变 ----------------
  printf("\n[D] 删表后参数读取结果不变（缓存不是真相源）\n");
  {
    const int64_t probe = editedIds[5];
    json::JsonValue beforeParams;
    std::string loadErr;
    check(xmp.loadParams(probe, &beforeParams, &loadErr), "删表前读参数");
    const std::string beforeJson = db::DevelopSettingsDao::canonicalJson(beforeParams);

    std::string dropErr;
    check(database.exec("DROP TABLE develop_settings;", &dropErr), "删表成功（" + dropErr + "）");

    json::JsonValue afterParams;
    check(xmp.loadParams(probe, &afterParams, &loadErr), "删表后读参数成功（不依赖缓存）");
    check(db::DevelopSettingsDao::canonicalJson(afterParams) == beforeJson,
          "D：删表前后参数逐字相等");

    db::DevelopSettingsRow row;
    bool found = true;
    std::string getErr;
    check(dao.get(probe, &row, &found, &getErr), "表缺失时 get() 降级不报错");
    check(!found, "表缺失时 found=false（按 sidecar 路径走）");

    // 复原：版本回退 → 迁移 → 重建，保证后续批次可以继续用这个库
    db::writeDbVersion(&database, db::kDbVersionFilesTable, nullptr);
    int code = db::kMigrationOk;
    db::MigrationReport rep;
    check(db::runVersionedMigrations(&database, &code, &rep) && rep.devTableCreated == 1,
          "复原：003 重新建表");
    db::RebuildReport rb;
    std::atomic<bool> cancel(false);
    check(dao.rebuildFromSidecar(&xmp, &rb, &cancel, nullptr, &err), "复原：重建缓存");
  }

  printf("\n=== 结果：%s（失败 %d 项）===\n", gFailures == 0 ? "全部通过" : "存在失败", gFailures);
  return gFailures == 0 ? 0 : 1;
}
