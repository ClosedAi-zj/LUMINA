// 地图域服务：GPS 聚合、地点详情、定位编辑、收藏、未定位列表、
// 离线瓦片 + GPX 导入/择点写入（WP-A4，@since 1.1.0）
#ifndef LUMINA_MAP_MAP_SERVICE_H_
#define LUMINA_MAP_MAP_SERVICE_H_

#include <cstdint>
#include <string>
#include <vector>

#include "core/config.h"
#include "db/database.h"
#include "db/photo_dao.h"
#include "json/json_value.h"
#include "rpc/dispatcher.h"
#include "xmp/xmp_service.h"

namespace lumina {
namespace map {

// GPX 点位（航点 wpt + 轨迹点 trkpt 统一形状；导入顺序即点位编号）
struct GpxPoint {
  double lat = 0.0;
  double lng = 0.0;
  double ele = 0.0;
  int64_t timeMs = 0;
  std::string name;
  bool hasEle = false;
  bool hasTime = false;
  bool hasName = false;
};

// 设计要点：地点不依赖预置数据。真实用户导入的照片只会带 EXIF GPS，
// 所以聚合一律从 photos.gps_lat/gps_lng 现算，命中后再把地点登记进 locations 表拿稳定 id
// （收藏、详情、定位编辑都需要 id）。
class MapService {
 public:
  MapService(core::Config* config, db::Database* db, db::PhotoDao* photos, xmp::XmpService* xmp);

  // {viewport?, zoom, onlyLocated?} → [{locationId, name, lat, lng, count, cluster, timeSpan}]
  json::JsonValue getLocations(const json::JsonValue& params, rpc::RpcError* err);
  // {locationId} → {name, lat, lng, altitude, count, timeSpan, favorite, samplePhotoId}
  json::JsonValue getLocationDetail(const json::JsonValue& params, rpc::RpcError* err);
  // {ids, lat, lng, altitude?, locationName?} → {updated}
  json::JsonValue setPhotoLocation(const json::JsonValue& params, rpc::RpcError* err);
  // {ids} → {updated}
  json::JsonValue clearPhotoLocation(const json::JsonValue& params, rpc::RpcError* err);
  // {page} → QueryResult
  json::JsonValue listUnlocated(const json::JsonValue& params, rpc::RpcError* err);
  // {locationId, favorite} → {}
  json::JsonValue toggleFavorite(const json::JsonValue& params, rpc::RpcError* err);

  // ---- WP-A4（协议 §5.13 @since 1.1.0） ----
  // {z, x, y} → {path, source:"local"|"online"}；目录瓦片 {home}/maptiles/{z}/{x}/{y}.png
  // 优先；未命中且 settings.mapOnlineFallback=true 时在线下载并缓存，否则 1001（零静默）
  json::JsonValue getTile(const json::JsonValue& params, rpc::RpcError* err);
  // {path} → {count, points:[{lat,lng,ele?,time?,name?}]}；点位驻留内存供 applyGpxPoints
  json::JsonValue importGpx(const json::JsonValue& params, rpc::RpcError* err);
  // {photoIds:[], pointIndexes:[]} → {updated, locationId, sidecarFailed, warnings}
  // photoIds[i] ← 最近一次 importGpx 的第 pointIndexes[i] 个点（按位配对）
  json::JsonValue applyGpxPoints(const json::JsonValue& params, rpc::RpcError* err);

 private:
  // 按坐标 + 名称登记地点，返回稳定 id（已存在则复用）
  int64_t ensureLocation(const std::string& name, double lat, double lng, double alt,
                         std::string* err);
  // 瓦片文件路径（目录瓦片布局）
  std::string tilePath(int z, int x, int y) const;
  // 在线下载瓦片并缓存（WinHTTP 动态加载；失败给出明确 err）
  bool downloadTile(int z, int x, int y, const std::string& dest, std::string* err);
  // 单张照片定位写入（DB + sidecar 零静默），复用 setPhotoLocation 同一套红线
  bool writeLocation(int64_t photoId, double lat, double lng, double alt,
                     const std::string& name, int64_t* locationId, bool* sidecarOk,
                     std::string* sidecarErr);

  core::Config* config_;
  db::Database* db_;
  db::PhotoDao* photos_;
  xmp::XmpService* xmp_;
  std::vector<GpxPoint> gpxPoints_;  // 最近一次 importGpx 的点位集（会话级）
};

}  // namespace map
}  // namespace lumina

#endif  // LUMINA_MAP_MAP_SERVICE_H_
