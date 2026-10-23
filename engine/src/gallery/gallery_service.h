// 客户画廊：把画册发布成可离线打开的静态站（{home}/gallery/{token}/）+ 收藏反馈闭环
// 红线：原片零写入（只读原片/缩略图缓存），只写 {home}/gallery/** 与 DB；
//       任何失败（画册为空、预览生成失败、目录不可写）一律返回中文 message 的 RpcError，绝不静默。
// 定位：静态站门禁是「本地预览级门禁，非服务端鉴权」（协议 §9-Q3 已确认）。
#ifndef LUMINA_GALLERY_GALLERY_SERVICE_H_
#define LUMINA_GALLERY_GALLERY_SERVICE_H_

#include <cstdint>
#include <string>
#include <vector>

#include "core/config.h"
#include "db/album_dao.h"
#include "db/database.h"
#include "db/photo_dao.h"
#include "json/json_value.h"
#include "rpc/dispatcher.h"
#include "thumb/thumbnail_service.h"

namespace lumina {
namespace gallery {

// 静态站里的一张照片（已复制到 images/{id}.jpg）
struct GalleryPhoto {
  int64_t photoId = 0;
  int width = 0;
  int height = 0;
  std::string caption;  // 文件名（不含扩展名），作为可见标题
};

// 发布 / 撤回 / 列表 / 反馈导入 / 反馈读回（ipc-protocol §5.14，5 个方法）
class GalleryService {
 public:
  GalleryService(core::Config* config, db::Database* db, db::PhotoDao* photos,
                 db::AlbumDao* albums, thumb::ThumbnailService* thumbs);

  // 生成静态站并登记发布信息。返回 {token, staticDir, expiresAt, previewUrl, photos, missing}
  json::JsonValue publish(const json::JsonValue& params, rpc::RpcError* err);
  // 下线：删静态站目录 + 清 albums.published_*（幂等；反馈数据保留）
  json::JsonValue revoke(const json::JsonValue& params, rpc::RpcError* err);
  // 已发布列表：[{albumId, token, previewUrl, staticDir, expiresAt, expired, stats:{...}}]
  json::JsonValue listPublished(const json::JsonValue& params, rpc::RpcError* err);
  // 导入客户从静态站导出的收藏包（JSON）
  json::JsonValue importFeedback(const json::JsonValue& params, rpc::RpcError* err);
  // 读回反馈，与 importFeedback 对称
  json::JsonValue getFeedback(const json::JsonValue& params, rpc::RpcError* err);

 private:
  // gallery_meta / gallery_feedback 两张辅助表：schema.sql 由主理人独占，本服务首次使用时
  // 用 CREATE TABLE IF NOT EXISTS 幂等建表（不改 schema.sql、不写 schema_meta 版本号）。
  bool ensureTables(std::string* err);
  // 预览图：优先命中缩略图缓存，否则真解码生成到 {thumbs}/{size}/{id}.jpg
  bool preparePreview(int64_t photoId, std::string* previewPath, std::string* err);
  json::JsonValue feedbackStats(int64_t albumId, rpc::RpcError* err);

  core::Config* config_;
  db::Database* db_;
  db::PhotoDao* photos_;
  db::AlbumDao* albums_;
  thumb::ThumbnailService* thumbs_;
};

// 静态站索引页（独立函数便于人眼核对/单测；纯内联 CSS/JS，无外链）
std::string buildIndexHtml(int64_t albumId, const std::string& title,
                           const std::string& customerName,
                           const std::vector<GalleryPhoto>& photos, const std::string& token,
                           const std::string& passwordHash, int64_t expiresAtMs);

// sha256(password + token) 的小写十六进制串（静态站前端用 Web Crypto 算同一个值比对）
std::string sha256Hex(const std::string& text);

// 加密强度随机 token（advapi32!SystemFunction036，48 个十六进制字符 = 24 字节熵）
std::string secureToken(size_t hexChars);

}  // namespace gallery
}  // namespace lumina

#endif  // LUMINA_GALLERY_GALLERY_SERVICE_H_
