// 地图域实现：全部聚合现算，不依赖预置地点数据
#include "map/map_service.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <string>
#include <utility>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "core/logger.h"
#include "core/str_util.h"

namespace lumina {
namespace map {

namespace {

// zoom → 经纬度网格位数。低 zoom 聚成大区块，高 zoom 逐点。
int gridDecimals(int zoom) {
  if (zoom <= 0) return 0;  // 全球：10° 级别区块
  if (zoom <= 4) return 0;
  if (zoom <= 6) return 1;
  if (zoom <= 8) return 2;
  if (zoom <= 11) return 3;
  return 5;
}

std::string defaultLocationName(double lat, double lng) {
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%.3f, %.3f", lat, lng);
  return std::string(buf);
}

}  // namespace

MapService::MapService(core::Config* config, db::Database* db, db::PhotoDao* photos,
                       xmp::XmpService* xmp)
    : config_(config), db_(db), photos_(photos), xmp_(xmp) {}

int64_t MapService::ensureLocation(const std::string& name, double lat, double lng, double alt,
                                  std::string* err) {
  // 先按名称复用：同名地点在多次聚合之间必须拿到同一个 id，
  // 否则收藏状态会每刷新一次地图就丢一次。
  {
    auto q = db_->prepareCached("mapFindLocation", "SELECT id FROM locations WHERE name=? LIMIT 1;",
                                err);
    if (!q || !q->valid()) return 0;
    q->bindText(1, name);
    if (q->step(err)) {
      int64_t id = q->columnInt64(0);
      q->reset();
      return id;
    }
    q->reset();
  }
  auto ins = db_->prepareCached(
      "mapInsertLocation",
      "INSERT INTO locations(name, lat, lng, alt, favorite) VALUES(?,?,?,?,0) RETURNING id;", err);
  if (!ins || !ins->valid()) return 0;
  ins->bindText(1, name);
  ins->bindDouble(2, lat);
  ins->bindDouble(3, lng);
  ins->bindDouble(4, alt);
  if (!ins->step(err)) return 0;
  int64_t id = ins->columnInt64(0);
  ins->reset();
  return id;
}

json::JsonValue MapService::getLocations(const json::JsonValue& params, rpc::RpcError* err) {
  int zoom = static_cast<int>(params.get("zoom").asInt(8));
  const int decimals = gridDecimals(zoom > 0 ? zoom : 8);
  char roundExpr[32];
  std::snprintf(roundExpr, sizeof(roundExpr), "%d", decimals);

  const json::JsonValue& viewport = params.get("viewport");
  std::string where = " WHERE deleted=0 AND is_missing=0 AND gps_lat IS NOT NULL AND gps_lng IS NOT NULL";
  bool hasViewport = viewport.isObject() && viewport.contains("minLat") &&
                     viewport.contains("maxLat") && viewport.contains("minLng") &&
                     viewport.contains("maxLng");
  if (hasViewport) {
    where += " AND gps_lat BETWEEN ? AND ? AND gps_lng BETWEEN ? AND ?";
  }

  std::string sql = "SELECT ROUND(gps_lat, " + std::string(roundExpr) +
                    ") AS blat, ROUND(gps_lng, " + std::string(roundExpr) +
                    ") AS blng, COUNT(*) AS cnt, MIN(captured_at), MAX(captured_at), "
                    "AVG(gps_alt), MAX(location) FROM photos" +
                    where + " GROUP BY blat, blng ORDER BY cnt DESC LIMIT 500;";

  std::string dbErr;
  auto stmt = db_->prepareCached("mapGetLocations", sql, &dbErr);
  if (!stmt || !stmt->valid()) {
    *err = rpc::RpcError(rpc::kDbError, "map.getLocations prepare failed: " + dbErr);
    return json::JsonValue::nullValue();
  }
  int pos = 1;
  if (hasViewport) {
    stmt->bindDouble(pos++, viewport.get("minLat").asDouble());
    stmt->bindDouble(pos++, viewport.get("maxLat").asDouble());
    stmt->bindDouble(pos++, viewport.get("minLng").asDouble());
    stmt->bindDouble(pos++, viewport.get("maxLng").asDouble());
  }

  // 同名地点去重：相邻网格可能都命中同一个 location 名（如"雾浓顶"跨网格），
  // 若每条都各自 ensureLocation，会因按 name 复用而返回同一个 id ——
  // 响应里出现两条相同 locationId 不同坐标，前端口 locationId 作选中键即 key 冲突。
  // 处理：同名网格并入首条 cluster（count 累加、timeSpan 取并集），保证 id 在响应内唯一。
  struct ClusterAgg {
    double lat = 0, lng = 0, alt = 0;
    int64_t count = 0, from = 0, to = 0;
  };
  std::vector<std::pair<std::string, ClusterAgg>> clusters;
  std::map<std::string, size_t> nameIndex;
  while (stmt->step(&dbErr)) {
    double lat = stmt->columnDouble(0);
    double lng = stmt->columnDouble(1);
    int64_t count = stmt->columnInt64(2);
    int64_t from = stmt->columnIsNull(3) ? 0 : stmt->columnInt64(3);
    int64_t to = stmt->columnIsNull(4) ? 0 : stmt->columnInt64(4);
    double alt = stmt->columnIsNull(5) ? 0.0 : stmt->columnDouble(5);
    std::string name = stmt->columnIsNull(6) ? std::string() : stmt->columnText(6);
    if (name.empty()) name = defaultLocationName(lat, lng);

    auto it = nameIndex.find(name);
    if (it != nameIndex.end()) {
      ClusterAgg& agg = clusters[it->second].second;
      agg.count += count;
      agg.from = agg.from > 0 && from > 0 ? std::min(agg.from, from)
                     : (agg.from > 0 ? agg.from : from);
      agg.to = std::max(agg.to, to);
      continue;
    }
    ClusterAgg agg;
    agg.lat = lat;
    agg.lng = lng;
    agg.alt = alt;
    agg.count = count;
    agg.from = from;
    agg.to = to;
    nameIndex.emplace(name, clusters.size());
    clusters.emplace_back(name, agg);
  }
  stmt->reset();

  json::JsonValue out = json::JsonValue::arrayValue();
  for (const auto& kv : clusters) {
    const std::string& name = kv.first;
    const ClusterAgg& agg = kv.second;
    std::string locErr;
    int64_t locationId = ensureLocation(name, agg.lat, agg.lng, agg.alt, &locErr);
    if (locationId == 0 && locErr.empty()) locErr = "ensureLocation returned 0";

    json::JsonValue item = json::JsonValue::objectValue();
    item.setItem("locationId", locationId);
    item.setItem("name", name);
    item.setItem("lat", agg.lat);
    item.setItem("lng", agg.lng);
    item.setItem("altitude", agg.alt);
    item.setItem("count", agg.count);
    // zoom 较低时相邻点会被并进同一网格，此时标 cluster 让前端口径一致
    item.setItem("cluster", agg.count > 1 || decimals < 3);
    json::JsonValue span = json::JsonValue::objectValue();
    span.setItem("from", agg.from);
    span.setItem("to", agg.to);
    item.setItem("timeSpan", span);
    out.addItem(item);
  }
  return out;
}

json::JsonValue MapService::getLocationDetail(const json::JsonValue& params, rpc::RpcError* err) {
  int64_t locationId = params.get("locationId").asInt();
  if (locationId <= 0) {
    *err = rpc::RpcError(rpc::kInvalidParams, "params.locationId is required");
    return json::JsonValue::nullValue();
  }
  std::string name;
  double lat = 0.0, lng = 0.0, alt = 0.0;
  int favorite = 0;
  {
    std::string dbErr;
    auto q = db_->prepareCached(
        "mapLocationById", "SELECT name, lat, lng, alt, favorite FROM locations WHERE id=?;", &dbErr);
    if (!q || !q->valid()) {
      *err = rpc::RpcError(rpc::kDbError, "map.getLocationDetail prepare failed: " + dbErr);
      return json::JsonValue::nullValue();
    }
    q->bindInt64(1, locationId);
    if (!q->step(&dbErr)) {
      q->reset();
      *err = rpc::RpcError(rpc::kFileNotFound, "location not found: " + std::to_string(locationId));
      return json::JsonValue::nullValue();
    }
    name = q->columnIsNull(0) ? std::string() : q->columnText(0);
    lat = q->columnDouble(1);
    lng = q->columnDouble(2);
    alt = q->columnIsNull(3) ? 0.0 : q->columnDouble(3);
    favorite = q->columnInt(4);
    q->reset();
  }

  int64_t count = 0, from = 0, to = 0, sampleId = 0;
  {
    std::string dbErr;
    // 按地点名匹配照片（导入时 location 来自 EXIF/手动标注），退化时按坐标容差匹配
    auto q = db_->prepareCached("mapLocationStats",
                                "SELECT COUNT(*), MIN(captured_at), MAX(captured_at), MIN(id) "
                                "FROM photos WHERE deleted=0 AND is_missing=0 AND "
                                "(location=? OR (gps_lat BETWEEN ? AND ? AND gps_lng BETWEEN ? AND ?));",
                                &dbErr);
    if (q && q->valid()) {
      q->bindText(1, name);
      q->bindDouble(2, lat - 0.01);
      q->bindDouble(3, lat + 0.01);
      q->bindDouble(4, lng - 0.01);
      q->bindDouble(5, lng + 0.01);
      if (q->step(&dbErr)) {
        count = q->columnInt64(0);
        from = q->columnIsNull(1) ? 0 : q->columnInt64(1);
        to = q->columnIsNull(2) ? 0 : q->columnInt64(2);
        sampleId = q->columnIsNull(3) ? 0 : q->columnInt64(3);
      }
      q->reset();
    }
  }

  json::JsonValue out = json::JsonValue::objectValue();
  out.setItem("locationId", locationId);
  out.setItem("name", name);
  out.setItem("lat", lat);
  out.setItem("lng", lng);
  out.setItem("altitude", alt);
  out.setItem("count", count);
  out.setItem("favorite", favorite != 0);
  out.setItem("samplePhotoId", sampleId);
  json::JsonValue span = json::JsonValue::objectValue();
  span.setItem("from", from);
  span.setItem("to", to);
  out.setItem("timeSpan", span);
  return out;
}

json::JsonValue MapService::setPhotoLocation(const json::JsonValue& params, rpc::RpcError* err) {
  double lat = params.get("lat").asDouble(999.0);
  double lng = params.get("lng").asDouble(999.0);
  if (lat > 90.0 || lat < -90.0 || lng > 180.0 || lng < -180.0) {
    *err = rpc::RpcError(rpc::kInvalidParams, "params.lat/lng 必须是合法经纬度");
    return json::JsonValue::nullValue();
  }
  const json::JsonValue& ids = params.get("ids");
  if (!ids.isArray() || ids.size() == 0) {
    *err = rpc::RpcError(rpc::kInvalidParams, "params.ids must be a non-empty array");
    return json::JsonValue::nullValue();
  }
  double alt = params.get("altitude").asDouble(0.0);
  std::string name = params.get("locationName").asString();
  if (name.empty()) name = defaultLocationName(lat, lng);

  std::string locErr;
  int64_t locationId = ensureLocation(name, lat, lng, alt, &locErr);
  if (locationId <= 0) {
    *err = rpc::RpcError(rpc::kDbError, "map.setPhotoLocation ensureLocation failed: " + locErr);
    return json::JsonValue::nullValue();
  }

  int64_t updated = 0;
  int64_t sidecarFailed = 0;
  json::JsonValue warnings = json::JsonValue::arrayValue();
  std::string dbErr;
  auto upd = db_->prepareCached("mapSetPhotoLocation",
                               "UPDATE photos SET gps_lat=?, gps_lng=?, gps_alt=?, location_id=?, "
                               "location=? WHERE id=? RETURNING id;",
                               &dbErr);
  if (!upd || !upd->valid()) {
    *err = rpc::RpcError(rpc::kDbError, "map.setPhotoLocation prepare failed: " + dbErr);
    return json::JsonValue::nullValue();
  }
  for (const auto& item : ids.arrayItems()) {
    int64_t photoId = item.asInt(0);
    if (photoId <= 0) continue;
    upd->bindDouble(1, lat);
    upd->bindDouble(2, lng);
    upd->bindDouble(3, alt);
    upd->bindInt64(4, locationId);
    upd->bindText(5, name);
    upd->bindInt64(6, photoId);
    if (upd->step(&dbErr)) {
      ++updated;
      // 红线：原片零写入。定位信息同步进 sidecar 的 exif: 命名空间，
      // 换机器打开同一张照片时定位不丢。
      if (xmp_) {
        std::string xmpErr;
        if (!xmp_->setExifGps(photoId, lat, lng, alt, name, &xmpErr)) {
          // 零静默：DB 已写入但 sidecar 未同步属于"部分成功"，必须显式上报
          ++sidecarFailed;
          warnings.addItem("照片 " + std::to_string(photoId) + " 定位已入库，但 XMP sidecar 同步失败：" +
                           (xmpErr.empty() ? "未知原因" : xmpErr));
          logWarn("map", "exif gps sidecar sync failed for photo " + std::to_string(photoId) + ": " +
                             xmpErr);
        }
      }
    }
    upd->reset();
  }
  json::JsonValue out = json::JsonValue::objectValue();
  out.setItem("updated", updated);
  out.setItem("locationId", locationId);
  out.setItem("sidecarFailed", sidecarFailed);
  out.setItem("warnings", warnings);
  return out;
}

json::JsonValue MapService::clearPhotoLocation(const json::JsonValue& params, rpc::RpcError* err) {
  const json::JsonValue& ids = params.get("ids");
  if (!ids.isArray() || ids.size() == 0) {
    *err = rpc::RpcError(rpc::kInvalidParams, "params.ids must be a non-empty array");
    return json::JsonValue::nullValue();
  }
  int64_t updated = 0;
  int64_t sidecarFailed = 0;
  json::JsonValue warnings = json::JsonValue::arrayValue();
  std::string dbErr;
  auto upd = db_->prepareCached("mapClearPhotoLocation",
                               "UPDATE photos SET gps_lat=NULL, gps_lng=NULL, gps_alt=NULL, "
                               "location_id=NULL, location=NULL WHERE id=? RETURNING id;",
                               &dbErr);
  if (!upd || !upd->valid()) {
    *err = rpc::RpcError(rpc::kDbError, "map.clearPhotoLocation prepare failed: " + dbErr);
    return json::JsonValue::nullValue();
  }
  for (const auto& item : ids.arrayItems()) {
    int64_t photoId = item.asInt(0);
    if (photoId <= 0) continue;
    upd->bindInt64(1, photoId);
    if (upd->step(&dbErr)) {
      ++updated;
      if (xmp_) {
        std::string xmpErr;
        if (!xmp_->clearExifGps(photoId, &xmpErr)) {
          // 零静默：库里清掉了、sidecar 里还留着 = 下次导入会"复活"定位
          ++sidecarFailed;
          warnings.addItem("照片 " + std::to_string(photoId) + " 定位已从库中清除，但 XMP sidecar 清除失败：" +
                           (xmpErr.empty() ? "未知原因" : xmpErr));
          logWarn("map", "exif gps clear failed for photo " + std::to_string(photoId) + ": " + xmpErr);
        }
      }
    }
    upd->reset();
  }
  json::JsonValue out = json::JsonValue::objectValue();
  out.setItem("updated", updated);
  out.setItem("sidecarFailed", sidecarFailed);
  out.setItem("warnings", warnings);
  return out;
}

json::JsonValue MapService::listUnlocated(const json::JsonValue& params, rpc::RpcError* err) {
  const json::JsonValue& page = params.get("page");
  int64_t offset = page.isObject() ? page.get("offset").asInt(0) : 0;
  int64_t limit = page.isObject() ? page.get("limit").asInt(100) : 100;
  if (limit <= 0) limit = 100;
  if (limit > 500) limit = 500;

  int64_t total = 0;
  std::string dbErr;
  {
    auto c = db_->prepareCached("mapUnlocatedCount",
                                "SELECT COUNT(*) FROM photos WHERE deleted=0 AND "
                                "gps_lat IS NULL;",
                                &dbErr);
    if (c && c->valid() && c->step(&dbErr)) {
      total = c->columnInt64(0);
      c->reset();
    }
  }
  json::JsonValue items = json::JsonValue::arrayValue();
  {
    auto q = db_->prepareCached("mapUnlocatedPage",
                                "SELECT id, file_name, file_path, captured_at, rating "
                                "FROM photos WHERE deleted=0 AND gps_lat IS NULL "
                                "ORDER BY captured_at DESC LIMIT ? OFFSET ?;",
                                &dbErr);
    if (q && q->valid()) {
      q->bindInt64(1, limit);
      q->bindInt64(2, offset);
      while (q->step(&dbErr)) {
        json::JsonValue it = json::JsonValue::objectValue();
        it.setItem("id", q->columnInt64(0));
        it.setItem("fileName", q->columnText(1));
        it.setItem("filePath", q->columnText(2));
        it.setItem("capturedAt", q->columnIsNull(3) ? 0 : q->columnInt64(3));
        it.setItem("rating", q->columnInt(4));
        items.addItem(it);
      }
      q->reset();
    }
  }
  json::JsonValue out = json::JsonValue::objectValue();
  out.setItem("items", items);
  out.setItem("total", total);
  out.setItem("offset", offset);
  out.setItem("limit", limit);
  return out;
}

json::JsonValue MapService::toggleFavorite(const json::JsonValue& params, rpc::RpcError* err) {
  int64_t locationId = params.get("locationId").asInt();
  if (locationId <= 0) {
    *err = rpc::RpcError(rpc::kInvalidParams, "params.locationId is required");
    return json::JsonValue::nullValue();
  }
  bool favorite = params.get("favorite").asBool();
  std::string dbErr;
  auto upd = db_->prepareCached("mapToggleFavorite",
                               "UPDATE locations SET favorite=? WHERE id=? RETURNING id;", &dbErr);
  if (!upd || !upd->valid()) {
    *err = rpc::RpcError(rpc::kDbError, "map.toggleFavorite prepare failed: " + dbErr);
    return json::JsonValue::nullValue();
  }
  upd->bindInt(1, favorite ? 1 : 0);
  upd->bindInt64(2, locationId);
  if (!upd->step(&dbErr)) {
    upd->reset();
    *err = rpc::RpcError(rpc::kFileNotFound, "location not found: " + std::to_string(locationId));
    return json::JsonValue::nullValue();
  }
  upd->reset();
  json::JsonValue out = json::JsonValue::objectValue();
  out.setItem("locationId", locationId);
  out.setItem("favorite", favorite);
  return out;
}

// ================= WP-A4：离线瓦片 + GPX（@since 1.1.0） =================

namespace {

// ---- WinHTTP 动态加载（本机 MinGW 无 winhttp 头/导入库，与 advapi32 同模式） ----
struct WinHttpApi {
  using Handle = void*;
  using FnOpen = Handle (__stdcall*)(const wchar_t*, unsigned long, const wchar_t*,
                                     const wchar_t*, unsigned long);
  using FnConnect = Handle (__stdcall*)(Handle, const wchar_t*, unsigned short, unsigned long);
  using FnOpenRequest = Handle (__stdcall*)(Handle, const wchar_t*, const wchar_t*,
                                            const wchar_t*, const wchar_t*, const wchar_t**,
                                            unsigned long);
  using FnSendRequest = int (__stdcall*)(Handle, const wchar_t*, unsigned long, void*,
                                         unsigned long, unsigned long, unsigned long long);
  using FnReceive = int (__stdcall*)(Handle, void*);
  using FnQueryData = int (__stdcall*)(Handle, unsigned long*);
  using FnRead = int (__stdcall*)(Handle, void*, unsigned long, unsigned long*);
  using FnClose = int (__stdcall*)(Handle);

  HMODULE dll = nullptr;
  FnOpen open = nullptr;
  FnConnect connect = nullptr;
  FnOpenRequest openRequest = nullptr;
  FnSendRequest sendRequest = nullptr;
  FnReceive receive = nullptr;
  FnQueryData queryData = nullptr;
  FnRead read = nullptr;
  FnClose close = nullptr;

  bool load() {
    if (dll) return open != nullptr;
    dll = LoadLibraryA("winhttp.dll");
    if (!dll) return false;
    open = reinterpret_cast<FnOpen>(GetProcAddress(dll, "WinHttpOpen"));
    connect = reinterpret_cast<FnConnect>(GetProcAddress(dll, "WinHttpConnect"));
    openRequest = reinterpret_cast<FnOpenRequest>(GetProcAddress(dll, "WinHttpOpenRequest"));
    sendRequest = reinterpret_cast<FnSendRequest>(GetProcAddress(dll, "WinHttpSendRequest"));
    receive = reinterpret_cast<FnReceive>(GetProcAddress(dll, "WinHttpReceiveResponse"));
    queryData = reinterpret_cast<FnQueryData>(GetProcAddress(dll, "WinHttpQueryDataAvailable"));
    read = reinterpret_cast<FnRead>(GetProcAddress(dll, "WinHttpReadData"));
    close = reinterpret_cast<FnClose>(GetProcAddress(dll, "WinHttpCloseHandle"));
    return open && connect && openRequest && sendRequest && receive && queryData && read && close;
  }
};

const unsigned long kWinhttpAccessNoProxy = 1;     // WINHTTP_ACCESS_TYPE_NO_PROXY
const unsigned long kWinhttpFlagSecure = 0x00800000;  // WINHTTP_FLAG_SECURE

std::string utf8ToWideBuf(const std::string& s, std::wstring* out) {
  if (s.empty()) return std::string();
  int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
  if (n <= 0) return "utf8 conversion failed";
  out->resize(static_cast<size_t>(n));
  MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), &(*out)[0], n);
  return std::string();
}

// GPX 小型扫描解析：抓 <wpt>/<trkpt> 的 lat/lon 属性与 ele/time/name 子元素。
// GPX 是扁平结构，不值得为此引入 XML 库；解析失败（无任何点位）时报错，不吞。
int64_t parseMsFromLocal(const std::string& text);  // 定义见本文件下方（先声明后用）

bool parseGpx(const std::string& xml, std::vector<GpxPoint>* out, std::string* err) {
  size_t pos = 0;
  while (true) {
    size_t wpt = xml.find("<wpt ", pos);
    size_t trk = xml.find("<trkpt ", pos);
    size_t tagPos;
    bool isWaypoint;
    if (wpt != std::string::npos && (trk == std::string::npos || wpt < trk)) {
      tagPos = wpt;
      isWaypoint = true;
    } else if (trk != std::string::npos) {
      tagPos = trk;
      isWaypoint = false;
    } else {
      break;
    }
    (void)isWaypoint;
    size_t tagEnd = xml.find('>', tagPos);
    if (tagEnd == std::string::npos) break;
    std::string openTag = xml.substr(tagPos, tagEnd - tagPos + 1);

    GpxPoint p;
    auto attr = [&openTag](const char* name) -> std::string {
      std::string key = std::string(name) + "=\"";
      size_t k = openTag.find(key);
      if (k == std::string::npos) return std::string();
      size_t v = openTag.find('"', k + key.size());
      if (v == std::string::npos) return std::string();
      return openTag.substr(k + key.size(), v - k - key.size());
    };
    try {
      p.lat = std::stod(attr("lat"));
      p.lng = std::stod(attr("lon"));
    } catch (...) {
      pos = tagEnd;
      continue;
    }
    // 元素体：到 </wpt>/</trkpt> 或自闭合 "/>" 为止
    size_t bodyEnd = xml.find("</trkpt>", tagEnd);
    size_t bodyEndW = xml.find("</wpt>", tagEnd);
    if (bodyEnd == std::string::npos || (bodyEndW != std::string::npos && bodyEndW < bodyEnd)) {
      bodyEnd = bodyEndW;
    }
    size_t selfClose = openTag.find("/>") != std::string::npos
                           ? tagEnd
                           : std::string::npos;  // 自闭合无子元素
    std::string body =
        (selfClose != std::string::npos || bodyEnd == std::string::npos)
            ? std::string()
            : xml.substr(tagEnd + 1, bodyEnd - tagEnd - 1);
    auto childText = [&body](const char* tag) -> std::string {
      std::string open = std::string("<") + tag + ">";
      std::string close = std::string("</") + tag + ">";
      size_t a = body.find(open);
      if (a == std::string::npos) return std::string();
      size_t b = body.find(close, a);
      if (b == std::string::npos) return std::string();
      return body.substr(a + open.size(), b - a - open.size());
    };
    std::string ele = childText("ele");
    if (!ele.empty()) {
      try {
        p.ele = std::stod(ele);
        p.hasEle = true;
      } catch (...) {
      }
    }
    std::string t = childText("time");
    if (!t.empty()) {
      // ISO8601 → epoch ms（仅取本地语义的 Y-M-D H:M:S；GPX 一般是 UTC Z 后缀）
      int y = 0, mo = 0, d = 0, hh = 0, mm = 0, ss = 0;
      if (sscanf(t.c_str(), "%d-%d-%dT%d:%d:%d", &y, &mo, &d, &hh, &mm, &ss) == 6) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d", y, mo, d, hh, mm, ss);
        p.timeMs = parseMsFromLocal(std::string(buf));
        p.hasTime = true;
      }
    }
    std::string name = childText("name");
    if (!name.empty()) {
      p.name = name;
      p.hasName = true;
    }
    out->push_back(p);
    pos = (bodyEnd == std::string::npos) ? tagEnd + 1 : bodyEnd + 1;
  }
  if (out->empty()) {
    if (err) *err = "GPX 解析结果为空（未找到 wpt/trkpt 点位）";
    return false;
  }
  return true;
}

// "YYYY-MM-DD HH:MM:SS" → epoch ms（UTC；与 mock/generator 同口径，本地实现避免跨模块依赖）
int64_t parseMsFromLocal(const std::string& text) {
  int y = 0, mo = 0, d = 0, h = 0, mi = 0, s = 0;
  if (sscanf(text.c_str(), "%d-%d-%d %d:%d:%d", &y, &mo, &d, &h, &mi, &s) < 3) return 0;
  if (y < 1970 || mo < 1 || mo > 12 || d < 1 || d > 31) return 0;
  static const int kDaysBeforeMonth[13] = {0,   0,   31,  59,  90,  120, 151,
                                           181, 212, 243, 273, 304, 334};
  int64_t days = 365 * (y - 1970) + (y - 1969) / 4;
  days += kDaysBeforeMonth[mo] + (d - 1);
  if (mo > 2 && ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0)) ++days;
  int64_t secs = days * 86400 + h * 3600 + mi * 60 + s;
  return secs * 1000;
}

}  // namespace

std::string MapService::tilePath(int z, int x, int y) const {
  return joinPath(config_->homeDir(),
                  "maptiles/" + std::to_string(z) + "/" + std::to_string(x) + "/" +
                      std::to_string(y) + ".png");
}

bool MapService::downloadTile(int z, int x, int y, const std::string& dest, std::string* err) {
  static WinHttpApi api;  // 进程级加载一次
  if (!api.load()) {
    if (err) *err = "winhttp.dll 不可用，无法在线下载瓦片";
    return false;
  }
  const std::string host = "tile.openstreetmap.org";
  const std::string path = "/" + std::to_string(z) + "/" + std::to_string(x) + "/" +
                           std::to_string(y) + ".png";
  std::wstring wHost, wPath, wAgent;
  utf8ToWideBuf(host, &wHost);
  utf8ToWideBuf(path, &wPath);
  utf8ToWideBuf("LUMINA/1.1.0 (desktop tile cache)", &wAgent);

  WinHttpApi::Handle session = api.open(wAgent.c_str(), kWinhttpAccessNoProxy, nullptr, nullptr, 0);
  if (!session) {
    if (err) *err = "WinHttpOpen failed (error " + std::to_string(GetLastError()) + ")";
    return false;
  }
  WinHttpApi::Handle conn = api.connect(session, wHost.c_str(), 443, 0);
  WinHttpApi::Handle req = nullptr;
  if (conn) {
    req = api.openRequest(conn, L"GET", wPath.c_str(), nullptr, nullptr, nullptr,
                          kWinhttpFlagSecure);
  }
  bool ok = false;
  std::string data;
  if (req) {
    const wchar_t* kAgentHeader = L"User-Agent: LUMINA/1.1.0 (desktop tile cache)\r\n";
    if (api.sendRequest(req, kAgentHeader, static_cast<unsigned long>(wcslen(kAgentHeader)),
                        nullptr, 0, 0, 0) &&
        api.receive(req, nullptr)) {
      unsigned long avail = 0;
      while (api.queryData(req, &avail) && avail > 0) {
        size_t before = data.size();
        data.resize(before + avail);
        unsigned long got = 0;
        if (!api.read(req, &data[before], avail, &got)) break;
        data.resize(before + got);
        if (got == 0) break;
      }
    }
  }
  // 校验 PNG 签名（瓦片固定为 PNG；防把 404 HTML 当瓦片缓存）
  const unsigned char kPngSig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
  if (data.size() > 8 && std::equal(kPngSig, kPngSig + 8, data.begin())) {
    std::string tmp = dest + ".tmp";
    std::string writeErr;
    if (writeFileAtomic(tmp, data, &writeErr)) {
      if (fileExists(dest)) removeFile(dest);
      std::string moveErr;
      if (copyFileTo(tmp, dest, &moveErr)) {
        removeFile(tmp);
        ok = true;
      } else if (err) {
        *err = "瓦片缓存转正失败: " + moveErr;
      }
    } else if (err) {
      *err = "瓦片写盘失败: " + writeErr;
    }
  } else if (err) {
    *err = "在线瓦片响应无效（非 PNG 或为空，z=" + std::to_string(z) + "/x=" + std::to_string(x) +
           "/y=" + std::to_string(y) + "）";
  }
  if (req) api.close(req);
  if (conn) api.close(conn);
  api.close(session);
  return ok;
}

json::JsonValue MapService::getTile(const json::JsonValue& params, rpc::RpcError* err) {
  int z = static_cast<int>(params.get("z").asInt(-1));
  int x = static_cast<int>(params.get("x").asInt(-1));
  int y = static_cast<int>(params.get("y").asInt(-1));
  if (z < 0 || z > 22 || x < 0 || y < 0) {
    *err = rpc::RpcError(rpc::kInvalidParams, "params.z/x/y 必须是合法瓦片坐标（z 0..22）");
    return json::JsonValue::nullValue();
  }
  int64_t n = static_cast<int64_t>(1) << z;
  if (x >= n || y >= n) {
    *err = rpc::RpcError(rpc::kInvalidParams, "params.x/y 超出 z=" + std::to_string(z) +
                                                 " 层的瓦片范围（0.." + std::to_string(n - 1) +
                                                 "）");
    return json::JsonValue::nullValue();
  }
  const std::string path = tilePath(z, x, y);
  if (fileExists(path) && fileSize(path) > 0) {
    json::JsonValue out = json::JsonValue::objectValue();
    out.setItem("path", path);
    out.setItem("source", "local");
    return out;
  }
  // 未命中：按 settings 开关决定是否回退在线（离线优先，默认关闭）
  const bool onlineAllowed = config_->settings().get("mapOnlineFallback").asBool(false);
  if (!onlineAllowed) {
    *err = rpc::RpcError(rpc::kFileNotFound,
                         "瓦片未缓存且在线回退未开启（settings.mapOnlineFallback），z=" +
                             std::to_string(z) + "/x=" + std::to_string(x) + "/y=" +
                             std::to_string(y));
    return json::JsonValue::nullValue();
  }
  makeDirs(joinPath(config_->homeDir(), "maptiles/" + std::to_string(z) + "/" + std::to_string(x)));
  std::string dlErr;
  if (!downloadTile(z, x, y, path, &dlErr)) {
    *err = rpc::RpcError(rpc::kIoError, "在线瓦片下载失败: " + dlErr);
    return json::JsonValue::nullValue();
  }
  logInfo("map", "tile downloaded: z=" + std::to_string(z) + "/x=" + std::to_string(x) +
                     "/y=" + std::to_string(y));
  json::JsonValue out = json::JsonValue::objectValue();
  out.setItem("path", path);
  out.setItem("source", "online");
  return out;
}

json::JsonValue MapService::importGpx(const json::JsonValue& params, rpc::RpcError* err) {
  const std::string path = params.get("path").asString();
  if (path.empty()) {
    *err = rpc::RpcError(rpc::kInvalidParams, "params.path is required");
    return json::JsonValue::nullValue();
  }
  if (!fileExists(path)) {
    *err = rpc::RpcError(rpc::kFileNotFound, "GPX 文件不存在: " + path);
    return json::JsonValue::nullValue();
  }
  std::string text;
  std::string readErr;
  if (!readFile(path, &text, &readErr)) {
    *err = rpc::RpcError(rpc::kIoError, "GPX 读取失败: " + readErr);
    return json::JsonValue::nullValue();
  }
  std::vector<GpxPoint> points;
  std::string parseErr;
  if (!parseGpx(text, &points, &parseErr)) {
    *err = rpc::RpcError(rpc::kInvalidParams, parseErr);
    return json::JsonValue::nullValue();
  }
  json::JsonValue arr = json::JsonValue::arrayValue();
  for (const GpxPoint& p : points) {
    json::JsonValue item = json::JsonValue::objectValue();
    item.setItem("lat", p.lat);
    item.setItem("lng", p.lng);
    if (p.hasEle) item.setItem("ele", p.ele);
    if (p.hasTime) item.setItem("time", p.timeMs);
    if (p.hasName) item.setItem("name", p.name);
    arr.addItem(item);
  }
  gpxPoints_ = points;  // 驻留内存，applyGpxPoints 按 pointIndexes 取用
  json::JsonValue out = json::JsonValue::objectValue();
  out.setItem("count", static_cast<int64_t>(points.size()));
  out.setItem("points", arr);
  return out;
}

bool MapService::writeLocation(int64_t photoId, double lat, double lng, double alt,
                               const std::string& name, int64_t* locationId, bool* sidecarOk,
                               std::string* sidecarErr) {
  *sidecarOk = true;
  std::string locErr;
  int64_t locId = ensureLocation(name, lat, lng, alt, &locErr);
  if (locId <= 0) {
    if (sidecarErr) *sidecarErr = "ensureLocation failed: " + locErr;
    return false;
  }
  *locationId = locId;
  std::string dbErr;
  auto upd = db_->prepareCached("mapGpxWriteLocation",
                                "UPDATE photos SET gps_lat=?, gps_lng=?, gps_alt=?, location_id=?, "
                                "location=? WHERE id=? AND deleted=0 RETURNING id;",
                                &dbErr);
  if (!upd || !upd->valid()) {
    if (sidecarErr) *sidecarErr = "prepare failed: " + dbErr;
    return false;
  }
  upd->bindDouble(1, lat);
  upd->bindDouble(2, lng);
  upd->bindDouble(3, alt);
  upd->bindInt64(4, locId);
  upd->bindText(5, name);
  upd->bindInt64(6, photoId);
  bool inDb = upd->step(&dbErr);
  upd->reset();
  if (!inDb) {
    if (sidecarErr) *sidecarErr = "照片不存在或已删除: " + std::to_string(photoId);
    return false;
  }
  // 红线：原片零写入。定位同步进 sidecar 的 exif: 命名空间；失败必须显式上报（零静默）
  if (xmp_) {
    std::string xmpErr;
    if (!xmp_->setExifGps(photoId, lat, lng, alt, name, &xmpErr)) {
      *sidecarOk = false;
      if (sidecarErr) *sidecarErr = xmpErr;
    }
  }
  return true;
}

json::JsonValue MapService::applyGpxPoints(const json::JsonValue& params, rpc::RpcError* err) {
  const json::JsonValue& photoIds = params.get("photoIds");
  const json::JsonValue& pointIndexes = params.get("pointIndexes");
  if (!photoIds.isArray() || !pointIndexes.isArray() ||
      photoIds.size() != pointIndexes.size()) {
    *err = rpc::RpcError(rpc::kInvalidParams,
                         "params.photoIds/pointIndexes 必须是等长非空数组（按位配对）");
    return json::JsonValue::nullValue();
  }
  if (gpxPoints_.empty()) {
    *err = rpc::RpcError(rpc::kInvalidParams,
                         "没有可用的 GPX 点位：请先调用 map.importGpx");
    return json::JsonValue::nullValue();
  }
  int64_t updated = 0;
  int64_t sidecarFailed = 0;
  int64_t lastLocationId = 0;
  json::JsonValue warnings = json::JsonValue::arrayValue();
  for (size_t i = 0; i < photoIds.size(); ++i) {
    int64_t photoId = photoIds.at(i).asInt(0);
    int64_t idx = pointIndexes.at(i).asInt(-1);
    if (photoId <= 0) {
      warnings.addItem("第 " + std::to_string(i) + " 项 photoId 非法（" +
                       std::to_string(photoId) + "），已跳过");
      continue;
    }
    if (idx < 0 || idx >= static_cast<int64_t>(gpxPoints_.size())) {
      // 越界不中断（协议：越界索引计入 warnings）
      warnings.addItem("photoId " + std::to_string(photoId) + " 的点位下标越界（" +
                       std::to_string(idx) + "，共 " + std::to_string(gpxPoints_.size()) + " 点）");
      continue;
    }
    const GpxPoint& p = gpxPoints_[static_cast<size_t>(idx)];
    std::string name = p.hasName ? p.name : defaultLocationName(p.lat, p.lng);
    int64_t locId = 0;
    bool sidecarOk = true;
    std::string why;
    if (writeLocation(photoId, p.lat, p.lng, p.ele, name, &locId, &sidecarOk, &why)) {
      ++updated;
      lastLocationId = locId;
      if (!sidecarOk) {
        ++sidecarFailed;
        warnings.addItem("照片 " + std::to_string(photoId) + " 定位已入库，但 XMP sidecar 同步失败：" +
                         (why.empty() ? "未知原因" : why));
        logWarn("map", "gpx sidecar sync failed for photo " + std::to_string(photoId) + ": " + why);
      }
    } else {
      warnings.addItem("photoId " + std::to_string(photoId) + " 写入失败: " + why);
    }
  }
  json::JsonValue out = json::JsonValue::objectValue();
  out.setItem("updated", updated);
  out.setItem("locationId", lastLocationId);
  out.setItem("sidecarFailed", sidecarFailed);
  out.setItem("warnings", warnings);
  return out;
}

}  // namespace map
}  // namespace lumina
