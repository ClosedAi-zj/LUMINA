// M2 T2.2 · 版本化幂等迁移实现（002 四级表 + 回填）
//
// 幂等性设计（这是本文件的核心）：
//   * DDL 全部 IF NOT EXISTS；唯一不幂等的 ALTER 由 PRAGMA table_info 守卫；
//   * 回填以 photos.file_id IS NULL 为工作集 → 二次运行工作集为空 → 0 变更；
//   * schema_meta.db_version 只在成功后写；版本已达标则整体跳过（upToDate）。
// 事务：DDL 与回填分开提交（DDL 已幂等，回填分块提交以便取消时只丢当前块）。
#include "db/migrations.h"

#include <algorithm>
#include <string>
#include <vector>

#include "core/logger.h"
#include "core/str_util.h"

namespace lumina {
namespace db {
namespace {

constexpr int kBackfillChunk = 500;  // 每 500 行提交一次，取消时最多丢一块

// 单值查询（int64）。失败返回 -1 并写 err。
int64_t queryInt(Database* db, const std::string& sql, const std::string& errCtx, int* err) {
  auto stmt = db->prepareCached("q:" + errCtx, sql, nullptr);
  if (!stmt || !stmt->valid()) {
    if (err) *err = kMigrationBackfillFailed;
    logError("migrate", "query prepare failed: " + errCtx);
    return -1;
  }
  int64_t v = -1;
  if (stmt->step(nullptr)) v = stmt->columnInt64(0);
  stmt->reset();
  return v;
}

// ⚠ 性能纪律（QA 在 6c6224f 实测出的问题）：`Database::prepareCached` 自线程安全修复
// 起每次调用都**新建**语句（见 database.cpp 的说明，key 被显式忽略）。因此凡在
// 行级循环里调 prepareCached = 每行两次 sqlite3_prepare，实测 0.4 ms/行 →
// 18,192 张 ≈ 7 s 启动阻塞。回填/重建这类热循环必须把语句 prepare 一次后
// 在循环外持有（shared_ptr<Stmt>），循环内只 bind/step/reset 复用。

}  // namespace

bool tableHasColumn(Database* db, const std::string& table, const std::string& column) {
  if (!db || !db->isOpen()) return false;
  const std::string sql = "PRAGMA table_info(" + table + ");";
  auto stmt = db->prepareCached("pragma:" + table, sql, nullptr);
  if (!stmt || !stmt->valid()) return false;
  bool found = false;
  while (stmt->step(nullptr)) {
    // table_info 列：cid(0) name(1) type(2) notnull(3) dflt_value(4) pk(5)
    if (stmt->columnText(1) == column) {
      found = true;
      break;
    }
  }
  stmt->reset();
  return found;
}

int readDbVersion(Database* db) {
  if (!db || !db->isOpen()) return 0;
  const int64_t v = queryInt(db, "SELECT value FROM schema_meta WHERE key='db_version';",
                             "readDbVersion", nullptr);
  return v < 0 ? 0 : static_cast<int>(v);
}

bool writeDbVersion(Database* db, int version, int* err) {
  const std::string sql = "INSERT INTO schema_meta(key,value) VALUES('db_version','" +
                          std::to_string(version) +
                          "') ON CONFLICT(key) DO UPDATE SET value=excluded.value;";
  std::string e;
  if (!db->exec(sql, &e)) {
    if (err) *err = kMigrationDdlFailed;
    logError("migrate", "write db_version failed: " + e);
    return false;
  }
  return true;
}

// ---------------- 002：四级表 ----------------
bool applyMigration002FilesTable(Database* db, MigrationReport* report, std::atomic<bool>* cancel,
                                 const std::function<void(int, const std::string&)>& progress,
                                 int* err) {
  if (!db || !db->isOpen()) {
    if (err) *err = kMigrationNotOpen;
    return false;
  }
  MigrationReport local;
  MigrationReport& rep = report ? *report : local;

  // ① files 表（纯增量；CREATE TABLE IF NOT EXISTS 天然幂等）
  //    文件身份 = (folder_id, name)（架构 D8）。photos 表不动。
  const char* kCreateFiles =
      "CREATE TABLE IF NOT EXISTS files("
      "  id INTEGER PRIMARY KEY,"
      "  folder_id INT NOT NULL REFERENCES folders(id),"
      "  name TEXT NOT NULL,"
      "  format TEXT,"
      "  file_size INT,"
      "  md5 TEXT,"
      "  mtime INT,"
      "  missing INT DEFAULT 0,"
      "  health TEXT DEFAULT 'unchecked',"
      "  UNIQUE(folder_id, name)"
      ");";
  std::string e;
  if (!db->exec(kCreateFiles, &e)) {
    if (err) *err = kMigrationDdlFailed;
    rep.detail = "create files failed: " + e;
    logError("migrate", rep.detail);
    return false;
  }
  ++rep.statementsRun;

  // ② photos.file_id（ALTER 不幂等 → PRAGMA 守卫）
  if (!tableHasColumn(db, "photos", "file_id")) {
    if (!db->exec("ALTER TABLE photos ADD COLUMN file_id INT;", &e)) {
      if (err) *err = kMigrationDdlFailed;
      rep.detail = "alter photos.file_id failed: " + e;
      logError("migrate", rep.detail);
      return false;
    }
    ++rep.statementsRun;
    logInfo("migrate", "002: photos.file_id added");
  }

  // ③ 索引（IF NOT EXISTS 幂等）
  if (!db->exec("CREATE INDEX IF NOT EXISTS idx_photos_file ON photos(file_id);", &e)) {
    if (err) *err = kMigrationDdlFailed;
    rep.detail = "create idx_photos_file failed: " + e;
    logError("migrate", rep.detail);
    return false;
  }
  ++rep.statementsRun;
  // 反向索引：由 (folder_id, name) 反查文件行（ImageDao::ensureFileRow 热路径）
  if (!db->exec("CREATE INDEX IF NOT EXISTS idx_files_folder ON files(folder_id, name);", &e)) {
    if (err) *err = kMigrationDdlFailed;
    rep.detail = "create idx_files_folder failed: " + e;
    logError("migrate", rep.detail);
    return false;
  }
  ++rep.statementsRun;

  // ④ 回填：photos → files → photos.file_id
  const int64_t total = queryInt(db, "SELECT COUNT(*) FROM photos;", "countPhotos", err);
  if (total < 0) {
    if (err) *err = kMigrationBackfillFailed;
    rep.detail = "count photos failed";
    return false;
  }
  if (progress) progress(0, "回填文件行 0/" + std::to_string(total));

  // 行级热路径的 4 条语句全部 prepare 一次、循环内复用（性能纪律见文件头注释）：
  // 修复前每行 2 次 prepareCached（每次都新建语句），实测 0.4 ms/行 → 18,192 张 ≈ 7 s。
  auto scan = db->prepareCached(
      "backfillScan",
      "SELECT id, folder_id, file_name, file_id FROM photos WHERE id > ? ORDER BY id LIMIT ?;",
      &e);
  auto ins = db->prepareCached(
      "backfillInsert",
      "INSERT OR IGNORE INTO files(folder_id,name,format,file_size) VALUES(?,?,NULL,NULL);", &e);
  auto sel =
      db->prepareCached("backfillSelect", "SELECT id FROM files WHERE folder_id=? AND name=? "
                                          "LIMIT 1;",
                        &e);
  auto upd = db->prepareCached("backfillUpdate", "UPDATE photos SET file_id=? WHERE id=?;", &e);
  if (!scan || !scan->valid() || !ins || !ins->valid() || !sel || !sel->valid() || !upd ||
      !upd->valid()) {
    if (err) *err = kMigrationBackfillFailed;
    rep.detail = "backfill prepare failed: " + e;
    logError("migrate", rep.detail);
    return false;
  }
  struct Row {
    int64_t id;
    bool hasFolder;
    int64_t folderId;
    std::string name;
    bool hasFile;
    int64_t fileId;
  };

  const int64_t backfillT0 = nowMs();
  int64_t done = 0;
  int64_t lastId = 0;
  while (true) {
    if (cancel && cancel->load()) {
      rep.cancelled = true;
      rep.detail = "回填被取消（已完成 " + std::to_string(done) + "/" + std::to_string(total) +
                   "，已提交部分保持有效，下次启动继续）";
      logWarn("migrate", rep.detail);
      if (err) *err = kMigrationCancelled;
      return false;
    }
    // 分页扫 photos（按 id 递增，避免一次 load 全表）。
    // 语句在循环外 prepare 一次（性能纪律见文件头注释），分页参数走绑定。
    std::vector<Row> rows;
    {
      scan->bindInt64(1, lastId);
      scan->bindInt64(2, kBackfillChunk);
      while (scan->step(nullptr)) {
        Row r;
        r.id = scan->columnInt64(0);
        r.hasFolder = !scan->columnIsNull(1);
        r.folderId = scan->columnInt64(1);
        r.name = scan->columnText(2);
        r.hasFile = !scan->columnIsNull(3);
        r.fileId = scan->columnInt64(3);
        rows.push_back(r);
      }
      scan->reset();
    }
    if (rows.empty()) break;
    lastId = rows.back().id;

    if (!db->begin(&e)) {
      if (err) *err = kMigrationBackfillFailed;
      rep.detail = "backfill begin failed: " + e;
      logError("migrate", rep.detail);
      return false;
    }
    for (const Row& r : rows) {
      ++done;
      if (!r.hasFolder || r.folderId <= 0 || r.name.empty()) {
        ++rep.photosUnlinked;
        continue;
      }
      // files 行：INSERT OR IGNORE 后回查 id（幂等；冲突即已存在）。
      // ins/sel/upd 三个语句全部在循环外 prepare，这里只绑定与执行。
      ins->bindInt64(1, r.folderId);
      ins->bindText(2, r.name);
      ins->step(nullptr);
      ins->reset();
      ++rep.statementsRun;
      if (db->changes() > 0) ++rep.filesInserted;

      sel->bindInt64(1, r.folderId);
      sel->bindText(2, r.name);
      int64_t fileId = 0;
      if (sel->step(nullptr)) fileId = sel->columnInt64(0);
      sel->reset();
      if (fileId <= 0) {
        db->rollback(nullptr);
        if (err) *err = kMigrationBackfillFailed;
        rep.detail = "backfill select file id failed for photo " + std::to_string(r.id);
        logError("migrate", rep.detail);
        return false;
      }
      if (!r.hasFile || r.fileId != fileId) {
        upd->bindInt64(1, fileId);
        upd->bindInt64(2, r.id);
        upd->step(nullptr);
        upd->reset();
        ++rep.statementsRun;
        ++rep.photosLinked;
      }
    }
    if (!db->commit(&e)) {
      if (err) *err = kMigrationBackfillFailed;
      rep.detail = "backfill commit failed: " + e;
      logError("migrate", rep.detail);
      return false;
    }
    if (progress && total > 0) {
      const int pct = static_cast<int>(std::min<int64_t>(100, done * 100 / total));
      progress(pct, "回填文件行 " + std::to_string(done) + "/" + std::to_string(total));
    }
  }

  if (rep.photosUnlinked > 0) {
    // 零静默：无法归位的行必须留痕（不回滚已成功的部分）
    rep.detail = std::to_string(rep.photosUnlinked) + " 行 photos 缺少 folder_id/文件名，未建立文件身份";
    logWarn("migrate", rep.detail);
  }
  // 耗时留痕：迁移是启动阻塞路径，回填耗时必须可观测（QA 要求，防再次退化）
  const int64_t backfillMs = nowMs() - backfillT0;
  rep.durationMs = backfillMs;
  logInfo("migrate", "002 backfill: " + std::to_string(done) + " rows in " +
                         std::to_string(backfillMs) + " ms" +
                         (done > 0 ? " (" +
                                         std::to_string((backfillMs * 1000) / std::max<int64_t>(done, 1)) +
                                         " us/row)"
                                   : ""));
  if (progress) progress(100, "回填完成：files +" + std::to_string(rep.filesInserted) + "，关联 " +
                                 std::to_string(rep.photosLinked) + "，耗时 " +
                                 std::to_string(backfillMs) + " ms");
  return true;
}

// ---------------- 003：develop_settings 派生缓存 ----------------
// 只做 DDL（表 + 两个部分索引），**不扫盘、不读 sidecar**：
//   * db 层不碰文件系统（分层），回填由 develop_settings_dao::rebuildFromSidecar 执行；
//   * 表本身是可丢弃派生，先建空表不影响任何读路径的正确性（参数仍从 sidecar 读）；
//   * 全部 IF NOT EXISTS → 二次运行 0 变更（幂等）。
bool applyMigration003DevelopSettings(Database* db, MigrationReport* report, int* err) {
  if (!db || !db->isOpen()) {
    if (err) *err = kMigrationNotOpen;
    return false;
  }
  MigrationReport local;
  MigrationReport& rep = report ? *report : local;
  std::string e;

  const bool existed = tableHasColumn(db, "develop_settings", "photo_id");

  const char* kCreateDevSettings =
      "CREATE TABLE IF NOT EXISTS develop_settings("
      "  photo_id INTEGER PRIMARY KEY REFERENCES photos(id),"
      "  params_json TEXT NOT NULL,"
      "  params_hash TEXT NOT NULL,"
      "  schema_version INT,"
      "  sidecar_mtime INT,"
      "  node_count INT,"
      "  mask_count INT DEFAULT 0,"
      "  has_edits INT DEFAULT 0,"
      "  updated_at INT,"
      "  source TEXT DEFAULT 'xmp'"
      ");";
  if (!db->exec(kCreateDevSettings, &e)) {
    if (err) *err = kMigrationDdlFailed;
    rep.detail = "create develop_settings failed: " + e;
    logError("migrate", rep.detail);
    return false;
  }
  ++rep.statementsRun;
  if (!existed) rep.devTableCreated = 1;

  // 部分索引：只索引「已调整」/「含蒙版」的行，筛选才可能走索引（H5 附带要求）
  if (!db->exec(
          "CREATE INDEX IF NOT EXISTS idx_dev_edited ON develop_settings(photo_id) "
          "WHERE has_edits=1;",
          &e)) {
    if (err) *err = kMigrationDdlFailed;
    rep.detail = "create idx_dev_edited failed: " + e;
    logError("migrate", rep.detail);
    return false;
  }
  ++rep.statementsRun;
  if (!db->exec(
          "CREATE INDEX IF NOT EXISTS idx_dev_masks ON develop_settings(photo_id) "
          "WHERE mask_count>0;",
          &e)) {
    if (err) *err = kMigrationDdlFailed;
    rep.detail = "create idx_dev_masks failed: " + e;
    logError("migrate", rep.detail);
    return false;
  }
  ++rep.statementsRun;

  logInfo("migrate", "003: develop_settings ready" +
                         std::string(rep.devTableCreated ? " (created)" : " (already present)"));
  return true;
}

// ---------------- 版本化调度 ----------------
bool runVersionedMigrations(Database* db, int* err) {
  return runVersionedMigrations(db, err, nullptr, nullptr, nullptr);
}

bool runVersionedMigrations(Database* db, int* err, MigrationReport* report) {
  return runVersionedMigrations(db, err, report, nullptr, nullptr);
}

bool runVersionedMigrations(Database* db, int* err, MigrationReport* report,
                            std::atomic<bool>* cancel,
                            const std::function<void(int, const std::string&)>& progress) {
  if (err) *err = kMigrationOk;
  if (!db || !db->isOpen()) {
    if (err) *err = kMigrationNotOpen;
    logError("migrate", "database not open");
    return false;
  }
  MigrationReport local;
  MigrationReport& rep = report ? *report : local;
  rep.fromVersion = readDbVersion(db);
  rep.toVersion = rep.fromVersion;

  if (rep.fromVersion > kTargetDbVersion) {
    // 版本更高：不猜测、不丢弃（与 sidecar 的 1006 schemaTooNew 同纪律）
    if (err) *err = kMigrationVersionTooNew;
    rep.detail = "数据库版本 " + std::to_string(rep.fromVersion) + " 高于本程序支持的 " +
                 std::to_string(kTargetDbVersion) + "（请升级客户端）";
    logError("migrate", rep.detail);
    return false;
  }
  if (rep.fromVersion >= kTargetDbVersion) {
    rep.upToDate = true;
    rep.detail = "数据库已是最新版本 " + std::to_string(rep.fromVersion) + "，本次 0 变更";
    logInfo("migrate", rep.detail);
    return true;
  }

  // 002：四级表（Q1 硬约束：必须早于 develop_settings / history）
  if (rep.fromVersion < kDbVersionFilesTable) {
    if (!applyMigration002FilesTable(db, &rep, cancel, progress, err)) return false;
    rep.toVersion = kDbVersionFilesTable;
    if (!writeDbVersion(db, rep.toVersion, err)) return false;
    logInfo("migrate", "migrated to db_version=" + std::to_string(rep.toVersion) +
                           " (files +" + std::to_string(rep.filesInserted) + ", linked " +
                           std::to_string(rep.photosLinked) + ")");
  }
  // 003：develop_settings 派生缓存（只建表；回填由 DAO 的 rebuildFromSidecar 执行）
  if (rep.fromVersion < kDbVersionDevelopSettings) {
    if (!applyMigration003DevelopSettings(db, &rep, err)) return false;
    rep.toVersion = kDbVersionDevelopSettings;
    if (!writeDbVersion(db, rep.toVersion, err)) return false;
    logInfo("migrate", "migrated to db_version=" + std::to_string(rep.toVersion) +
                           " (develop_settings " +
                           std::string(rep.devTableCreated ? "created" : "present") + ")");
  }
  // 004 扩展点：T2.4（history）在此按同一模式续写，版本号取 kDbVersionHistory，
  // 写完后同步上调 kTargetDbVersion。
  return true;
}

}  // namespace db
}  // namespace lumina
