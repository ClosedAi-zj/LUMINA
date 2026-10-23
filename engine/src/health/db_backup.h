// lumina.db 自动备份与损坏自愈（WP-A2）
// - 每日一份快照（SQLite Online Backup API，WAL 下安全），保留最近 7 份，放 {home}/backups/
// - 启动时主库损坏（打开失败 / quick_check 不通过）→ 从最新备份恢复并记录日志（零静默）
#ifndef LUMINA_HEALTH_DB_BACKUP_H_
#define LUMINA_HEALTH_DB_BACKUP_H_

#include <atomic>
#include <string>
#include <thread>

#include "core/config.h"
#include "db/database.h"

namespace lumina {
namespace health {

class DbBackupService {
 public:
  static constexpr int kKeepCount = 7;          // 保留最近 7 份
  static constexpr int kPollIntervalSec = 600;  // 每日守护线程巡检间隔（10 分钟）

  explicit DbBackupService(core::Config* config, db::Database* db);

  // 备份目录：{home}/backups/
  std::string backupsDir() const;

  // 用独立连接对 dbPath 做 PRAGMA quick_check；健康返回 true。
  // 静态方法：自愈发生在主连接打开之前，不能依赖已打开的 Database。
  static bool checkFileIntegrity(const std::string& dbPath, std::string* report,
                                 std::string* err);

  // 立即做一份当日快照（同名覆盖），随后清理只保留最近 kKeepCount 份。
  // 成功时 outPath 返回备份文件路径。
  bool backupNow(std::string* outPath, std::string* err);

  // 从 backups 目录找最新一份覆盖主库（先删主库与 -wal/-shm）。
  // 没有任何备份时返回 false 并给出明确 err（零静默，由调用方决定是否继续启动）。
  bool restoreLatest(std::string* restoredPath, std::string* err);

  // 启动自愈入口：主库不存在 → 直接返回（全新安装）；
  // 主库损坏 → 尝试恢复，recovered=true 表示已从备份还原。
  // 返回 false 且 *recovered=false 且 err 非空 = 损坏且无备份可用（调用方记日志后照常 open，
  // 让 open 给出原始错误，而不是在这里吞掉）。
  static bool recoverIfCorrupt(const std::string& dbPath, const std::string& backupsDir,
                               bool* recovered, std::string* err);

  // 每日守护线程：巡检当日是否已有备份，跨天首次巡检时补一份。
  void startDaily();
  void stopDaily();

 private:
  core::Config* config_;
  db::Database* db_;
  std::atomic<bool> dailyRunning_{false};
  std::thread dailyThread_;
};

}  // namespace health
}  // namespace lumina

#endif  // LUMINA_HEALTH_DB_BACKUP_H_
