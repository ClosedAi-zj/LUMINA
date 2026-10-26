// M2 T2.2 · 版本化幂等迁移框架（Q1：四级表 P0 且最先做）
//
// 设计约束（架构 §3.3.1，不得改动）：
//  1) **不重建 photos 表**：photos 是 photos_fts 的外部内容表（content='photos' + 3 个
//     触发器），重建会让 FTS 索引与触发器全部失效。因此 002 只做纯增量：
//     CREATE TABLE IF NOT EXISTS files(...) + 守卫式 ALTER TABLE photos ADD COLUMN file_id
//     + CREATE INDEX IF NOT EXISTS idx_photos_file。
//  2) ALTER 不幂等 → 必须先 PRAGMA table_info(photos) 守卫（见 tableHasColumn）。
//  3) 回填进 job 语义：可取消、有进度、失败不静默（返回错误码 + 日志 + report 计数）。
//  4) 版本化：schema_meta.db_version 记录已达版本；重复运行必须 0 变更。
//  5) 003（develop_settings）/ 004（history）均为「绿地建表 + DAO + 迁移框架」；
//     004 由 T2.4 实装：只建表（可丢弃派生日志），数据由 EditSessionManager 按需写入。
#ifndef LUMINA_DB_MIGRATIONS_H_
#define LUMINA_DB_MIGRATIONS_H_

#include <atomic>
#include <functional>
#include <string>

#include "db/database.h"

namespace lumina {
namespace db {

// int* err 的取值（0 = 成功）。零静默：调用方必须把非 0 转成用户可见错误。
enum MigrationErrorCode {
  kMigrationOk = 0,
  kMigrationNotOpen = 1,
  kMigrationDdlFailed = 2,
  kMigrationBackfillFailed = 3,
  kMigrationVersionTooNew = 4,
  kMigrationCancelled = 5,
};

// 版本号常量。kTargetDbVersion 是本批实现到的最高版本。
constexpr int kDbVersionBaseline = 1;        // M1 关账（schema.sql 全量建表）
constexpr int kDbVersionFilesTable = 2;      // 002：四级表 files + photos.file_id（T2.2）
constexpr int kDbVersionDevelopSettings = 3;  // 003：develop_settings 派生缓存（T2.3）
constexpr int kDbVersionHistory = 4;          // 004：history_entries/state（T2.4）
constexpr int kTargetDbVersion = kDbVersionHistory;

// 迁移报告（供日志、job 进度文案与测试断言使用）
struct MigrationReport {
  int fromVersion = 0;
  int toVersion = 0;
  int filesInserted = 0;   // 002 新增的 files 行数（二次运行必须为 0）
  int photosLinked = 0;    // 002 回填后 photos.file_id 从空/旧值变为新值的行数
  int photosUnlinked = 0;  // folder_id 缺失等无法归位的行数（不静默：写 detail）
  int devTableCreated = 0;  // 003：本次是否新建了 develop_settings 表（0/1）
  int histTableCreated = 0;  // 004：本次是否新建了 history 表（0/1）
  int statementsRun = 0;   // 实际执行的 DDL/DML 语句数
  bool cancelled = false;
  bool upToDate = false;   // true = 版本已达标，本次 0 变更
  int64_t durationMs = 0;  // 002 回填耗时（启动阻塞路径必须可观测，QA 要求）
  std::string detail;      // 可读说明（失败原因 / 未归位行数等）
};

// 主入口（架构 §2.2 N8 逐字签名）。内部按 schema_meta.db_version 判断要跑哪几步。
bool runVersionedMigrations(Database* db, int* err);
// 带报告的重载（测试与 job 用）
bool runVersionedMigrations(Database* db, int* err, MigrationReport* report);
// 带取消标志与进度回调的重载（回填可能扫全表，必须可取消、有进度）
bool runVersionedMigrations(Database* db, int* err, MigrationReport* report,
                            std::atomic<bool>* cancel,
                            const std::function<void(int, const std::string&)>& progress);

// 002 单独可重入执行（幂等；供测试与后续版本复用）
bool applyMigration002FilesTable(Database* db, MigrationReport* report, std::atomic<bool>* cancel,
                                 const std::function<void(int, const std::string&)>& progress,
                                 int* err);

// 003：develop_settings 派生缓存（只建表 + 部分索引，**不扫盘**）。
// 回填由 develop_settings_dao::rebuildFromSidecar 显式执行（可取消、有进度、可放 job），
// 因为读 sidecar 属于 xmp 层职责，db 层不碰文件系统。
bool applyMigration003DevelopSettings(Database* db, MigrationReport* report, int* err);

// 004：history 派生日志（T2.4；只建表 + 索引，幂等）。
// 数据由 EditSessionManager 经 HistoryDao 按需写入（单 op 一事务，折叠为 checkpoint）。
bool applyMigration004History(Database* db, MigrationReport* report, int* err);

// PRAGMA table_info 守卫：table 是否已有 column（ALTER 不幂等，必须先问）
bool tableHasColumn(Database* db, const std::string& table, const std::string& column);
// 读 schema_meta.db_version（缺失返回 0）
int readDbVersion(Database* db);
// 写 schema_meta.db_version
bool writeDbVersion(Database* db, int version, int* err);

}  // namespace db
}  // namespace lumina

#endif  // LUMINA_DB_MIGRATIONS_H_
