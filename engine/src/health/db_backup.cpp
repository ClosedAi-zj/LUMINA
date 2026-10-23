// lumina.db 自动备份与损坏自愈实现（WP-A2）
#include "health/db_backup.h"

#include <algorithm>
#include <chrono>
#include <vector>

#include "core/logger.h"
#include "core/str_util.h"

namespace lumina {
namespace health {

namespace {

// 备份文件名：lumina-YYYYMMDD.db（按日命名，同名覆盖 = 每日一份；字典序即时间序）
std::string backupFileNameForToday() {
  // formatTimestamp → "YYYY-MM-DD HH:MM:SS.mmm"，取日期段去掉 '-'
  std::string day = formatTimestamp(nowMs()).substr(0, 10);
  day.erase(std::remove(day.begin(), day.end(), '-'), day.end());
  return "lumina-" + day + ".db";
}

bool looksLikeBackupName(const std::string& name) {
  return name.rfind("lumina-", 0) == 0 && name.size() > 10 &&
         name.compare(name.size() - 3, 3, ".db") == 0;
}

// 列出备份目录里的备份文件，按文件名降序（最新在前）
std::vector<std::string> listBackups(const std::string& dir) {
  std::vector<std::string> names;
  std::vector<DirEntry> entries;
  std::string err;
  if (!listDir(dir, &entries, &err)) return names;
  for (const DirEntry& e : entries) {
    if (e.isDir) continue;
    if (looksLikeBackupName(e.name)) names.push_back(e.name);
  }
  std::sort(names.begin(), names.end(), std::greater<std::string>());
  return names;
}

}  // namespace

DbBackupService::DbBackupService(core::Config* config, db::Database* db)
    : config_(config), db_(db) {}

std::string DbBackupService::backupsDir() const {
  return joinPath(config_->homeDir(), "backups");
}

bool DbBackupService::checkFileIntegrity(const std::string& dbPath, std::string* report,
                                         std::string* err) {
  sqlite3* raw = nullptr;
  int rc = sqlite3_open(dbPath.c_str(), &raw);
  if (rc != SQLITE_OK) {
    if (err) {
      *err = std::string("sqlite3_open failed: ") +
             (raw ? sqlite3_errmsg(raw) : "unknown error");
    }
    if (raw) sqlite3_close(raw);
    return false;
  }
  sqlite3_stmt* stmt = nullptr;
  rc = sqlite3_prepare_v2(raw, "PRAGMA quick_check;", -1, &stmt, nullptr);
  if (rc != SQLITE_OK) {
    if (err) *err = std::string("prepare quick_check failed: ") + sqlite3_errmsg(raw);
    sqlite3_close(raw);
    return false;
  }
  bool ok = false;
  if (sqlite3_step(stmt) == SQLITE_ROW) {
    const unsigned char* text = sqlite3_column_text(stmt, 0);
    std::string first = text ? reinterpret_cast<const char*>(text) : "";
    ok = (first == "ok");
    if (report) *report = first;
  } else {
    if (err) *err = "quick_check returned no row";
  }
  sqlite3_finalize(stmt);
  sqlite3_close(raw);
  return ok;
}

bool DbBackupService::backupNow(std::string* outPath, std::string* err) {
  if (!db_ || !db_->isOpen()) {
    if (err) *err = "database not open";
    return false;
  }
  const std::string dir = backupsDir();
  if (!makeDirs(dir)) {
    if (err) *err = "cannot create backups dir: " + dir;
    return false;
  }
  const std::string dest = joinPath(dir, backupFileNameForToday());
  // 先写临时文件再改名：中途失败不会留下半个当日备份顶替旧快照
  const std::string tmp = dest + ".tmp";

  sqlite3* destDb = nullptr;
  if (sqlite3_open(tmp.c_str(), &destDb) != SQLITE_OK) {
    if (err) *err = "cannot open backup target: " + tmp;
    if (destDb) sqlite3_close(destDb);
    removeFile(tmp);
    return false;
  }
  sqlite3_backup* handle = sqlite3_backup_init(destDb, "main", db_->handle(), "main");
  if (!handle) {
    if (err) *err = std::string("backup_init failed: ") + sqlite3_errmsg(destDb);
    sqlite3_close(destDb);
    removeFile(tmp);
    return false;
  }
  // -1 = 一次性复制整库（数百 MB 级秒级完成，备份在独立连接上不影响主连接读写）
  int rc = sqlite3_backup_step(handle, -1);
  if (rc != SQLITE_DONE) {
    if (err) *err = std::string("backup_step failed: ") + sqlite3_errmsg(db_->handle());
    sqlite3_backup_finish(handle);
    sqlite3_close(destDb);
    removeFile(tmp);
    return false;
  }
  sqlite3_backup_finish(handle);
  sqlite3_close(destDb);

  // 同名当日备份先删再改（Windows 上同名目标必须先移除）
  if (fileExists(dest)) removeFile(dest);
  std::string moveErr;
  if (!copyFileTo(tmp, dest, &moveErr)) {
    if (err) *err = "promote temp backup failed: " + moveErr;
    removeFile(tmp);
    return false;
  }
  removeFile(tmp);
  if (outPath) *outPath = dest;
  logInfo("db-backup", "snapshot written: " + dest);

  // 只保留最近 kKeepCount 份
  std::vector<std::string> backups = listBackups(dir);
  for (size_t i = static_cast<size_t>(kKeepCount); i < backups.size(); ++i) {
    std::string old = joinPath(dir, backups[i]);
    if (removeFile(old)) {
      logInfo("db-backup", "pruned old backup: " + backups[i]);
    } else {
      logWarn("db-backup", "prune failed: " + old);
    }
  }
  return true;
}

bool DbBackupService::restoreLatest(std::string* restoredPath, std::string* err) {
  const std::string dir = backupsDir();
  const std::string mainDb = config_->dbPath();
  std::vector<std::string> backups = listBackups(dir);
  if (backups.empty()) {
    if (err) *err = "no backup available in " + dir;
    return false;
  }
  const std::string latest = joinPath(dir, backups.front());
  // 主库连同 WAL/SHM 一并清掉（旧 wal 混入恢复后的库会再次报损坏）
  for (const char* suffix : {"", "-wal", "-shm"}) {
    std::string p = mainDb + suffix;
    if (fileExists(p) && !removeFile(p)) {
      if (err) *err = "cannot remove " + p;
      return false;
    }
  }
  std::string copyErr;
  if (!copyFileTo(latest, mainDb, &copyErr)) {
    if (err) *err = "copy backup failed: " + copyErr;
    return false;
  }
  if (restoredPath) *restoredPath = latest;
  logWarn("db-backup", "restored db from " + latest);
  return true;
}

bool DbBackupService::recoverIfCorrupt(const std::string& dbPath, const std::string& backupsDir,
                                       bool* recovered, std::string* err) {
  if (recovered) *recovered = false;
  if (!fileExists(dbPath)) {
    return true;  // 全新安装：无库可恢复
  }
  std::string report;
  std::string checkErr;
  if (checkFileIntegrity(dbPath, &report, &checkErr)) {
    return true;  // 主库健康
  }
  logError("db-backup", "db corrupt (quick_check: " + (report.empty() ? checkErr : report) +
                            "), attempting restore");
  std::vector<std::string> backups = listBackups(backupsDir);
  if (backups.empty()) {
    if (err) *err = "db corrupt and no backup available in " + backupsDir;
    return false;
  }
  const std::string latest = joinPath(backupsDir, backups.front());
  for (const char* suffix : {"", "-wal", "-shm"}) {
    std::string p = dbPath + suffix;
    if (fileExists(p) && !removeFile(p)) {
      if (err) *err = "cannot remove " + p;
      return false;
    }
  }
  std::string copyErr;
  if (!copyFileTo(latest, dbPath, &copyErr)) {
    if (err) *err = "copy backup failed: " + copyErr;
    return false;
  }
  if (recovered) *recovered = true;
  logWarn("db-backup", "restored db from " + latest);
  return true;
}

void DbBackupService::startDaily() {
  if (dailyRunning_.load()) return;
  dailyRunning_.store(true);
  dailyThread_ = std::thread([this]() {
    while (dailyRunning_.load()) {
      // 分段睡眠：stopDaily 时最多 1 秒内退出
      for (int i = 0; i < kPollIntervalSec && dailyRunning_.load(); ++i) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
      }
      if (!dailyRunning_.load()) break;
      const std::string today = backupFileNameForToday();
      if (fileExists(joinPath(backupsDir(), today))) continue;  // 当日已备份
      std::string outPath;
      std::string bErr;
      if (backupNow(&outPath, &bErr)) {
        logInfo("db-backup", "daily backup done: " + outPath);
      } else {
        // 巡检失败不吞：记日志，下一个巡检周期重试
        logWarn("db-backup", "daily backup failed: " + bErr);
      }
    }
  });
}

void DbBackupService::stopDaily() {
  if (!dailyRunning_.exchange(false)) return;
  if (dailyThread_.joinable()) dailyThread_.join();
}

}  // namespace health
}  // namespace lumina
