// 文件系统实时监测（ReadDirectoryChangesW）：缺失/恢复/修改 → event.fs.changed
#ifndef LUMINA_HEALTH_WATCHER_H_
#define LUMINA_HEALTH_WATCHER_H_

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "core/config.h"
#include "db/health_dao.h"
#include "db/photo_dao.h"
#include "json/json_value.h"

namespace lumina {
namespace health {

class HealthWatcher {
 public:
  using NotifyFn = std::function<void(const std::string& method, const json::JsonValue& params)>;

  HealthWatcher(core::Config* config, db::PhotoDao* photos, db::HealthDao* health, NotifyFn notify);
  ~HealthWatcher();

  // 对全部库根启动监测；失败返回 false（不阻塞启动）
  bool start(std::string* err);
  void stop();

 private:
  void watchRoot(int64_t rootId, const std::string& rootPath);
  void handleChange(int64_t rootId, const std::string& path, const char* change);

  core::Config* config_;
  db::PhotoDao* photos_;
  db::HealthDao* health_;
  NotifyFn notify_;
  std::vector<std::thread> threads_;
  std::atomic<bool> running_;
  std::atomic<int64_t> stopCounter_;

  // 同路径事件去抖：批量写入（导入、mock 重建、导出）时 ReadDirectoryChangesW 会为
  // 同一个文件连发 ADDED/REMOVED/MODIFIED，若每条都查库+改状态，会产生
  // "missing → restored → missing" 抖动并把生成耗时拖慢数倍。
  std::mutex debounceMutex_;
  std::unordered_map<std::string, std::pair<std::string, int64_t>> lastEvent_;
};

}  // namespace health
}  // namespace lumina

#endif  // LUMINA_HEALTH_WATCHER_H_
