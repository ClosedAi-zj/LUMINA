// XMP 版本迁移器：启动扫描 SchemaVersion 分布，<当前版本静默自动升级
#ifndef LUMINA_XMP_MIGRATOR_H_
#define LUMINA_XMP_MIGRATOR_H_

#include <atomic>
#include <string>

#include "core/config.h"
#include "db/database.h"
#include "db/photo_dao.h"
#include "json/json_value.h"
#include "rpc/dispatcher.h"
#include "xmp/xmp_document.h"

namespace lumina {
namespace xmp {

class XmpService;  // 前向声明：仅持有指针，避免头文件循环包含

class XmpMigrator {
 public:
  // xmp 非空：migrateOne 全程持 XmpService 同款 per-photo 写锁，与编辑会话/地图定位写入
  // 串行化（v1.1.1：修复后台全量迁移与实时编辑的 sidecar 写竞争，复用现有同步机制）。
  XmpMigrator(core::Config* config, db::PhotoDao* photos, db::Database* db,
              XmpService* xmp);

  // xmp.migrationStatus
  json::JsonValue status(rpc::RpcError* err);
  // 迁移单个 photo 的 sidecar（读→补 SchemaVersion→字节保留写回，全程持 per-photo 写锁）
  bool migrateOne(int64_t photoId, std::string* err);
  // T1.2：文档级迁移核心（v1→v2，幂等；无 DB 依赖，供测试直接调用）。
  // current >= 2 时 no-op 返回 true；current > 2 视为过新跳过（不猜测丢弃）。
  static bool migrateDocument(XmpDocument* doc, std::string* err);
  // 全量扫描并迁移（幂等）。stop 非空时每张之间检查停止标志，供后台线程优雅退出
  // （退出流程置位后处理完当前张即收尾，单张仅毫秒级，不会拖慢关闭）。
  bool migrateAll(int* migrated, std::string* err, const std::atomic<bool>* stop = nullptr);

 private:
  core::Config* config_;
  db::PhotoDao* photos_;
  db::Database* db_;
  XmpService* xmp_;
};

}  // namespace xmp
}  // namespace lumina

#endif  // LUMINA_XMP_MIGRATOR_H_
