// {home} 目录布局、settings.json 读写、库根目录管理
#ifndef LUMINA_CORE_CONFIG_H_
#define LUMINA_CORE_CONFIG_H_

#include <mutex>
#include <string>
#include <vector>

#include "json/json_value.h"

namespace lumina {
namespace core {

class Config {
 public:
  Config() : port_(0) {}

  // 初始化 home 目录（自动建 db/cache/logs/gallery 子目录）
  bool init(const std::string& homeDir, std::string* err);
  bool load(std::string* err);
  bool save(std::string* err);

  // ---- 目录布局 ----
  std::string homeDir() const;
  std::string dbPath() const;
  std::string cacheDir() const;
  std::string thumbsDir() const;
  std::string previewsDir() const;
  std::string rendersDir() const;
  std::string logsDir() const;
  std::string galleryDir() const;
  std::string backupsDir() const;  // lumina.db 快照（WP-A2：{home}/backups，保留 7 份）
  std::string settingsPath() const;
  std::string lockPath() const;

  // ---- 设置项 ----
  std::vector<std::string> libraryRoots() const;
  void setLibraryRoots(const std::vector<std::string>& roots);
  std::string backupTarget() const;
  void setBackupTarget(const std::string& path);
  int thumbCacheQuotaMB() const;
  std::string theme() const;
  int port() const { return port_; }
  void setPort(int port) { port_ = port; }

  // 整个 settings 对象（system.getSettings 直接返回）
  json::JsonValue settings() const;
  // 合并 patch 后落盘（system.setSettings）
  bool mergePatch(const json::JsonValue& patch, std::string* err);

  // 最近导入来源（最多 5 条）
  std::vector<std::string> recentSources() const;
  void pushRecentSource(const std::string& path);

  // 单实例锁：同 home 目录只允许一个引擎进程
  bool acquireLock(std::string* err);
  void releaseLock();

 private:
  mutable std::mutex mutex_;
  std::string homeDir_;
  json::JsonValue settings_;
  int port_;
  std::string lockFilePath_;
  void* lockHandle_ = nullptr;
};

}  // namespace core
}  // namespace lumina

#endif  // LUMINA_CORE_CONFIG_H_
