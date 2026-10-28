// SQLite 打开（WAL）、schema 迁移、预编译语句缓存实现
#include "db/database.h"

#include <string>

#include "core/logger.h"
#include "core/str_util.h"
#include "db/migrations.h"

// 由 CMake 从 src/db/schema.sql 生成（见 CMakeLists.txt）
#include "schema_sql.inc"

namespace lumina {
namespace db {

namespace {
// M2 T2.2：目标库版本升到 2（四级表）。003/004 由 T2.3/T2.4 续写并上调本常量。
constexpr int kTargetSchemaVersion = kTargetDbVersion;
}  // namespace

Stmt::~Stmt() {
  if (stmt_) {
    sqlite3_finalize(stmt_);
    stmt_ = nullptr;
  }
}

bool Stmt::prepare(sqlite3* db, const std::string& sql, std::string* err) {
  if (stmt_) {
    sqlite3_finalize(stmt_);
    stmt_ = nullptr;
  }
  int rc = sqlite3_prepare_v2(db, sql.c_str(), static_cast<int>(sql.size()), &stmt_, nullptr);
  if (rc != SQLITE_OK) {
    if (err) *err = std::string("prepare failed: ") + sqlite3_errmsg(db) + " | " + sql;
    stmt_ = nullptr;
    return false;
  }
  return true;
}

void Stmt::reset() {
  if (stmt_) sqlite3_reset(stmt_);
}
void Stmt::clearBindings() {
  if (stmt_) sqlite3_clear_bindings(stmt_);
}

bool Stmt::bindInt(int index, int value) {
  return stmt_ && sqlite3_bind_int(stmt_, index, value) == SQLITE_OK;
}
bool Stmt::bindInt64(int index, int64_t value) {
  return stmt_ && sqlite3_bind_int64(stmt_, index, static_cast<sqlite3_int64>(value)) == SQLITE_OK;
}
bool Stmt::bindDouble(int index, double value) {
  return stmt_ && sqlite3_bind_double(stmt_, index, value) == SQLITE_OK;
}
bool Stmt::bindText(int index, const std::string& value) {
  return stmt_ && sqlite3_bind_text(stmt_, index, value.data(), static_cast<int>(value.size()),
                                    SQLITE_TRANSIENT) == SQLITE_OK;
}
bool Stmt::bindNull(int index) {
  return stmt_ && sqlite3_bind_null(stmt_, index) == SQLITE_OK;
}

bool Stmt::step(std::string* err) {
  if (!stmt_) {
    if (err) *err = "statement not prepared";
    return false;
  }
  int rc = sqlite3_step(stmt_);
  if (rc == SQLITE_ROW) return true;
  if (rc == SQLITE_DONE) {
    // 读写语义自动区分（关键修复）：
    // - 非查询语句（INSERT/UPDATE/DELETE/DDL）执行完成的信号就是 SQLITE_DONE，
    //   必须判为 true，否则所有写入都会被误判失败、不 reset，后续语句集体失效。
    // - 查询语句（有结果列）走到 DONE 表示没有更多行，必须返回 false 让 while 循环退出。
    return sqlite3_column_count(stmt_) == 0;
  }
  if (err) *err = std::string("step failed: ") + sqlite3_errmsg(sqlite3_db_handle(stmt_));
  return false;
}

int Stmt::columnInt(int col) const { return stmt_ ? sqlite3_column_int(stmt_, col) : 0; }
int64_t Stmt::columnInt64(int col) const {
  return stmt_ ? static_cast<int64_t>(sqlite3_column_int64(stmt_, col)) : 0;
}
double Stmt::columnDouble(int col) const { return stmt_ ? sqlite3_column_double(stmt_, col) : 0.0; }
std::string Stmt::columnText(int col) const {
  if (!stmt_) return std::string();
  const unsigned char* p = sqlite3_column_text(stmt_, col);
  if (!p) return std::string();
  int n = sqlite3_column_bytes(stmt_, col);
  return std::string(reinterpret_cast<const char*>(p), static_cast<size_t>(n));
}
bool Stmt::columnIsNull(int col) const {
  return stmt_ ? sqlite3_column_type(stmt_, col) == SQLITE_NULL : true;
}

Database::~Database() { close(); }

bool Database::open(const std::string& path, std::string* err) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (db_) return true;
  int rc = sqlite3_open(path.c_str(), &db_);
  if (rc != SQLITE_OK) {
    if (err) *err = std::string("sqlite open failed: ") + sqlite3_errmsg(db_);
    sqlite3_close(db_);
    db_ = nullptr;
    return false;
  }
  // WAL 支撑多读单写；busy_timeout 防瞬时锁冲突
  // 返回码必须检查：sqlite3_exec 失败时 msg 才被填充，只 free 不看的写法会让
  // 「foreign_keys 没打开」「WAL 降级成 delete 日志」这类问题完全隐形。
  char* msg = nullptr;
  auto execPragma = [&](const char* sql, bool fatal, const char* what) -> bool {
    msg = nullptr;
    const int rc = sqlite3_exec(db_, sql, nullptr, nullptr, &msg);
    std::string detail = (msg != nullptr) ? std::string(msg) : std::string();
    if (msg) {
      sqlite3_free(msg);
      msg = nullptr;
    }
    if (rc == SQLITE_OK) return true;
    const std::string line = std::string(what) + " 设置失败（rc=" + std::to_string(rc) + "）" +
                             (detail.empty() ? std::string() : ": " + detail);
    if (fatal) {
      logError("db", line);
    } else {
      // 只读介质 / 网络盘上 WAL 会失败，此时降级为默认日志模式是可接受的，
      // 但必须留痕：不能假装自己跑在 WAL 上。
      logWarn("db", line);
    }
    return false;
  };
  execPragma("PRAGMA journal_mode=WAL;", /*fatal=*/false, "journal_mode=WAL");
  execPragma("PRAGMA busy_timeout=5000;", /*fatal=*/false, "busy_timeout=5000");
  execPragma("PRAGMA synchronous=NORMAL;", /*fatal=*/false, "synchronous=NORMAL");
  // 外键是 schema 完整性的一部分（files/photos/albums 的 ON DELETE 级联依赖它）。
  // 打开失败会让删照片留下孤儿行，属于必须显式告警的降级，不能静默继续。
  const bool fkViaPragma = execPragma("PRAGMA foreign_keys=ON;", /*fatal=*/true, "foreign_keys=ON");
  // 启动日志输出实际生效值，便于事后验证（而非相信「我设置过」）
  {
    int fkOn = 0;
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, "PRAGMA foreign_keys;", -1, &st, nullptr) == SQLITE_OK) {
      if (sqlite3_step(st) == SQLITE_ROW) fkOn = sqlite3_column_int(st, 0);
      sqlite3_finalize(st);
    }
    std::string journalMode = "unknown";
    st = nullptr;
    if (sqlite3_prepare_v2(db_, "PRAGMA journal_mode;", -1, &st, nullptr) == SQLITE_OK) {
      if (sqlite3_step(st) == SQLITE_ROW) {
        const unsigned char* p = sqlite3_column_text(st, 0);
        if (p) journalMode = reinterpret_cast<const char*>(p);
      }
      sqlite3_finalize(st);
    }
    if (fkOn != 1) {
      logError("db", "PRAGMA foreign_keys 实际生效值为 " + std::to_string(fkOn) + "（期望 1）" +
                         (fkViaPragma ? "" : "，且设置语句本身失败"));
    }
    logInfo("db", "pragmas effective: journal_mode=" + journalMode +
                      ", foreign_keys=" + std::to_string(fkOn) + ", busy_timeout=5000");
  }
  logInfo("db", "opened " + path);
  return true;
}

void Database::close() {
  std::lock_guard<std::mutex> lock(mutex_);
  cache_.clear();
  if (db_) {
    // 必须先看返回码：sqlite3_close 在仍有未 finalize 的 Stmt / 未提交事务时返回
    // SQLITE_BUSY 且**不会关闭句柄**。原来无条件 db_=nullptr + logInfo("closed")
    // 等于日志谎报「已关闭」并泄漏句柄（WAL 文件也不会被正常收尾）。
    const int rc = sqlite3_close(db_);
    if (rc != SQLITE_OK) {
      // 不抛错、不阻塞退出（红线：退出路径不能被一个坏句柄拖住），但必须留痕
      logWarn("db", std::string("sqlite3_close failed (rc=") + std::to_string(rc) + "): " +
                        sqlite3_errmsg(db_) + " —— 仍有未释放的语句或未提交事务，句柄泄漏");
    } else {
      logInfo("db", "closed");
    }
    db_ = nullptr;
  }
}

bool Database::exec(const std::string& sql, std::string* err) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!db_) {
    if (err) *err = "database not open";
    return false;
  }
  char* msg = nullptr;
  int rc = sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, &msg);
  if (rc != SQLITE_OK) {
    if (err) *err = msg ? std::string("exec failed: ") + msg : std::string("exec failed");
    sqlite3_free(msg);
    return false;
  }
  return true;
}

bool Database::begin(std::string* err) { return exec("BEGIN IMMEDIATE;", err); }
bool Database::commit(std::string* err) { return exec("COMMIT;", err); }
bool Database::rollback(std::string* err) { return exec("ROLLBACK;", err); }

int64_t Database::lastInsertId() const {
  return db_ ? static_cast<int64_t>(sqlite3_last_insert_rowid(db_)) : 0;
}
int Database::changes() const { return db_ ? sqlite3_changes(db_) : 0; }

int Database::schemaVersion() const {
  if (!db_) return 0;
  sqlite3_stmt* stmt = nullptr;
  int version = 0;
  if (sqlite3_prepare_v2(db_, "SELECT value FROM schema_meta WHERE key='db_version'", -1, &stmt,
                         nullptr) == SQLITE_OK) {
    if (sqlite3_step(stmt) == SQLITE_ROW) {
      version = sqlite3_column_int(stmt, 0);
    }
  }
  if (stmt) sqlite3_finalize(stmt);
  return version;
}

bool Database::runMigrations(std::string* err) {
  // 阶段 1：全量幂等建表（schema.sql 是唯一事实源，CREATE ... IF NOT EXISTS）
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_) {
      if (err) *err = "database not open";
      return false;
    }
    char* msg = nullptr;
    int rc = sqlite3_exec(db_, kSchemaSql, nullptr, nullptr, &msg);
    if (rc != SQLITE_OK) {
      if (err) *err = msg ? std::string("schema failed: ") + msg : std::string("schema failed");
      sqlite3_free(msg);
      return false;
    }
  }
  // 阶段 2：版本化增量迁移（002 四级表；003/004 由 T2.3/T2.4 续写）。
  // 必须在锁外调用：runVersionedMigrations 走 Database::exec/begin/commit，
  // 它们自身要拿 mutex_（持锁调用会自死锁）。
  int migErr = kMigrationOk;
  MigrationReport report;
  if (!runVersionedMigrations(this, &migErr, &report)) {
    if (err) {
      *err = "versioned migration failed (code " + std::to_string(migErr) + "): " + report.detail;
    }
    logError("db", "versioned migration failed, code=" + std::to_string(migErr) + " " +
                       report.detail);
    return false;
  }
  // 阶段 3：写 schema_meta.db_version（幂等 upsert）
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_) {
      if (err) *err = "database not open";
      return false;
    }
    std::string upsert = "INSERT INTO schema_meta(key,value) VALUES('db_version','" +
                         std::to_string(kTargetSchemaVersion) +
                         "') ON CONFLICT(key) DO UPDATE SET value=excluded.value;";
    char* msg = nullptr;
    int rc = sqlite3_exec(db_, upsert.c_str(), nullptr, nullptr, &msg);
    if (rc != SQLITE_OK) {
      if (err) *err = msg ? std::string("schema_meta failed: ") + msg : std::string("schema_meta");
      sqlite3_free(msg);
      return false;
    }
  }
  logInfo("db", "schema migrated to version " + std::to_string(kTargetSchemaVersion) +
                    (report.upToDate ? " (already current)" : ""));
  return true;
}

std::shared_ptr<Stmt> Database::prepareCached(const std::string& key, const std::string& sql,
                                              std::string* err) {
  // 线程安全修复：原先按 key 缓存并返回**同一个** sqlite3_stmt 实例，
  // 而引擎是多线程的（mock 生成 / 导入 / fs watcher 同时访问 DB），
  // 共享语句在 bind 与 step 之间会被另一线程插队覆盖，
  // 表现为随机 "bind failed at 1"（实测 4,347 次 sidecar 写入全失败）。
  // SQLite prepare 成本很低（数十微秒），这里改为每次返回独立语句，彻底消除共享。
  (void)key;
  auto stmt = std::make_shared<Stmt>();
  if (!stmt->prepare(db_, sql, err)) return nullptr;
  return stmt;
}

}  // namespace db
}  // namespace lumina
