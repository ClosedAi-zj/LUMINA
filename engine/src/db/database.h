// SQLite 打开（WAL）、schema 迁移框架、预编译语句缓存
#ifndef LUMINA_DB_DATABASE_H_
#define LUMINA_DB_DATABASE_H_

#include <map>
#include <memory>
#include <mutex>
#include <string>

#include "sqlite/sqlite3.h"

namespace lumina {
namespace db {

// RAII 预编译语句
class Stmt {
 public:
  Stmt() : stmt_(nullptr) {}
  ~Stmt();

  Stmt(const Stmt&) = delete;
  Stmt& operator=(const Stmt&) = delete;

  bool prepare(sqlite3* db, const std::string& sql, std::string* err);
  bool valid() const { return stmt_ != nullptr; }
  void reset();
  void clearBindings();

  bool bindInt(int index, int value);
  bool bindInt64(int index, int64_t value);
  bool bindDouble(int index, double value);
  bool bindText(int index, const std::string& value);
  bool bindNull(int index);

  // 返回 true = 有一行数据（SQLITE_ROW）；对无结果列的写语句（INSERT/UPDATE/DELETE），
  // 执行完成（SQLITE_DONE）即返回 true。查询语句无更多行时返回 false。
  bool step(std::string* err = nullptr);

  int columnInt(int col) const;
  int64_t columnInt64(int col) const;
  double columnDouble(int col) const;
  std::string columnText(int col) const;
  bool columnIsNull(int col) const;

  sqlite3_stmt* raw() { return stmt_; }

 private:
  sqlite3_stmt* stmt_;
};

class Database {
 public:
  Database() : db_(nullptr) {}
  ~Database();

  bool open(const std::string& path, std::string* err);
  void close();
  bool isOpen() const { return db_ != nullptr; }
  sqlite3* handle() { return db_; }

  bool exec(const std::string& sql, std::string* err);
  bool begin(std::string* err);
  bool commit(std::string* err);
  bool rollback(std::string* err);
  int64_t lastInsertId() const;
  int changes() const;

  // 迁移：执行 schema.sql（幂等），并维护 schema_meta.db_version
  bool runMigrations(std::string* err);
  int schemaVersion() const;

  // 语句工厂（⚠ 名字里的 cached 已名不副实）：因线程安全修复（见 database.cpp
  // prepareCached 的说明），每次调用都**新建**语句，key 仅用于错误定位。
  // 行级热循环（迁移回填 / 缓存重建）不得依赖它做「缓存」——请在循环外
  // 持有返回的 shared_ptr<Stmt>，循环内 bind/step/reset 复用。
  std::shared_ptr<Stmt> prepareCached(const std::string& key, const std::string& sql,
                                      std::string* err);

 private:
  sqlite3* db_;
  std::mutex mutex_;
  std::map<std::string, std::shared_ptr<Stmt>> cache_;
};

}  // namespace db
}  // namespace lumina

#endif  // LUMINA_DB_DATABASE_H_
