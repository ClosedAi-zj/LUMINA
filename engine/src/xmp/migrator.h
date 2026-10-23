// XMP 版本迁移器：启动扫描 SchemaVersion 分布，<当前版本静默自动升级
#ifndef LUMINA_XMP_MIGRATOR_H_
#define LUMINA_XMP_MIGRATOR_H_

#include <string>

#include "core/config.h"
#include "db/database.h"
#include "db/photo_dao.h"
#include "json/json_value.h"
#include "rpc/dispatcher.h"

namespace lumina {
namespace xmp {

class XmpMigrator {
 public:
  XmpMigrator(core::Config* config, db::PhotoDao* photos, db::Database* db);

  // xmp.migrationStatus
  json::JsonValue status(rpc::RpcError* err);
  // 迁移单个 photo 的 sidecar（读→补 SchemaVersion→字节保留写回）
  bool migrateOne(int64_t photoId, std::string* err);
  // 全量扫描并迁移（幂等）
  bool migrateAll(int* migrated, std::string* err);

 private:
  core::Config* config_;
  db::PhotoDao* photos_;
  db::Database* db_;
};

}  // namespace xmp
}  // namespace lumina

#endif  // LUMINA_XMP_MIGRATOR_H_
