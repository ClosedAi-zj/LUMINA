// 备份与恢复：第二目录/NAS 替身（原片 + sidecar 一并）
#ifndef LUMINA_HEALTH_BACKUP_H_
#define LUMINA_HEALTH_BACKUP_H_

#include <functional>
#include <string>
#include <vector>

#include "core/config.h"
#include "core/job_manager.h"
#include "db/database.h"
#include "db/photo_dao.h"
#include "json/json_value.h"
#include "rpc/dispatcher.h"

namespace lumina {
namespace health {

class BackupService {
 public:
  using NotifyFn = std::function<void(const std::string& method, const json::JsonValue& params)>;

  BackupService(core::Config* config, db::PhotoDao* photos, db::Database* db, NotifyFn notify);

  // 全量备份 job
  void runBackup(JobContext& ctx);
  // 从备份恢复单张（原片 + sidecar）
  bool restorePhoto(int64_t photoId, std::string* restoredPath, std::string* err);
  json::JsonValue configJson();

  // WP-A5（@since 1.1.0）：{format:"csv"|"json", path?} → {path, count}
  // 对比主库与备份根（config.backupTarget，布局 {target}/{rootId}/{fileName}）差异：
  //   missing（库有未备份）/ extra（备份有多余文件）/ checksum_failed（大小或 MD5 不符）
  json::JsonValue exportDiffReport(const json::JsonValue& params, rpc::RpcError* err);

 private:
  core::Config* config_;
  db::PhotoDao* photos_;
  db::Database* db_;
  NotifyFn notify_;
};

}  // namespace health
}  // namespace lumina

#endif  // LUMINA_HEALTH_BACKUP_H_
