// 存储健康扫描：存在性检查 + MD5 批量校验（异常绝不吞掉）
#ifndef LUMINA_HEALTH_SCANNER_H_
#define LUMINA_HEALTH_SCANNER_H_

#include <functional>
#include <string>
#include <vector>

#include "core/config.h"
#include "core/job_manager.h"
#include "db/database.h"
#include "db/health_dao.h"
#include "db/photo_dao.h"
#include "json/json_value.h"

namespace lumina {
namespace health {

class HealthScanner {
 public:
  using NotifyFn = std::function<void(const std::string& method, const json::JsonValue& params)>;

  HealthScanner(core::Config* config, db::PhotoDao* photos, db::HealthDao* health, NotifyFn notify);

  // 启动兜底全量扫描（存在性）
  void runFullScan(JobContext& ctx);
  // 批量 MD5 校验；失败逐条发 event.health.issue
  void verifyPhotos(const std::vector<int64_t>& ids, JobContext& ctx);

 private:
  void markMissing(int64_t photoId, const std::string& path);
  void markOk(int64_t photoId);

  core::Config* config_;
  db::PhotoDao* photos_;
  db::HealthDao* health_;
  NotifyFn notify_;
};

}  // namespace health
}  // namespace lumina

#endif  // LUMINA_HEALTH_SCANNER_H_
