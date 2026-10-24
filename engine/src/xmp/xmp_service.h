// XMP 读写服务：字节保留编辑 + 原子写 + mtime 冲突检测 + per-photo 写串行化
#ifndef LUMINA_XMP_XMP_SERVICE_H_
#define LUMINA_XMP_XMP_SERVICE_H_

#include <map>
#include <memory>
#include <mutex>
#include <string>

#include "core/config.h"
#include "core/thread_pool.h"
#include "db/database.h"
#include "db/photo_dao.h"
#include "json/json_value.h"
#include "rpc/dispatcher.h"
#include "xmp/xmp_mapper.h"

namespace lumina {
namespace xmp {

class XmpService {
 public:
  XmpService(core::Config* config, db::PhotoDao* photos, db::Database* db,
             core::ThreadPool* pool);

  // photoPath → 同目录同名 .xmp（绝对路径，正斜杠）
  std::string sidecarPathFor(const std::string& photoPath) const;

  json::JsonValue read(int64_t photoId, rpc::RpcError* err);
  json::JsonValue write(int64_t photoId, const json::JsonValue& params, rpc::RpcError* err);
  json::JsonValue checkExternal(int64_t photoId, rpc::RpcError* err);
  json::JsonValue listNamespaces(int64_t photoId, rpc::RpcError* err);

  // 给编辑会话 / 同步 / 预设使用的内部接口
  bool loadParams(int64_t photoId, json::JsonValue* out, std::string* err);
  // force=true 跳过"外部修改检测"。仅用于 mock 重建模拟图库这类软件自有数据的写入，
  // 真实用户 sidecar 的写入必须保持 force=false（红线 R1：绝不覆盖用户手改的 XMP）。
  bool saveParams(int64_t photoId, const json::JsonValue& params, std::string* err,
                  bool force = false);
  bool loadParamsFromPath(const std::string& photoPath, const std::string& sidecarPath,
                          json::JsonValue* out, std::string* err);

  // 地图定位：写/清 sidecar 的 exif:GPS* 字段（原片零写入，红线 R1）。
  // UI 侧 map.setPhotoLocation 依赖它，换机器打开同一张照片时定位不丢。
  bool setExifGps(int64_t photoId, double lat, double lng, double alt, const std::string& name,
                  std::string* err);
  bool clearExifGps(int64_t photoId, std::string* err);

  // per-photo sidecar 写锁（saveParams/setExifGps/clearExifGps 内部使用的同一把）。
  // 引擎内部其他 sidecar 写方（如 XmpMigrator 后台全量迁移）复用它，
  // 保证所有 read-modify-write 路径按照片串行化：不丢更新、不写坏正在编辑的 sidecar。
  std::shared_ptr<std::mutex> photoWriteMutex(int64_t photoId);

 private:

  core::Config* config_;
  db::PhotoDao* photos_;
  db::Database* db_;
  core::ThreadPool* pool_;
  std::mutex mutexMap_;
  std::map<int64_t, std::shared_ptr<std::mutex>> writeMutexes_;
};

}  // namespace xmp
}  // namespace lumina

#endif  // LUMINA_XMP_XMP_SERVICE_H_
