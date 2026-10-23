// 客户画廊实现：静态站生成 + 发布登记 + 收藏反馈闭环
// 依赖：core::Config（home 目录布局）、db::Database（辅助表）、db::AlbumDao（画册成员/发布字段）、
//       db::PhotoDao（照片元数据）、thumb::ThumbnailService（真解码预览图）。
#include "gallery/gallery_service.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include "core/logger.h"
#include "core/str_util.h"

namespace lumina {
namespace gallery {
namespace {

// 画廊大图尺寸：1024px 长边，客户在网页上够看，单张 ~150KB（本地预览级）
constexpr int kGalleryImageSize = 1024;
// 画廊站点目录内的子目录名
const char* kImagesDir = "images";

// ---------- 文本处理 ----------

std::string htmlEscape(const std::string& in) {
  std::string out;
  out.reserve(in.size());
  for (char c : in) {
    switch (c) {
      case '&': out += "&amp;"; break;
      case '<': out += "&lt;"; break;
      case '>': out += "&gt;"; break;
      case '"': out += "&quot;"; break;
      case '\'': out += "&#39;"; break;
      default: out.push_back(c); break;
    }
  }
  return out;
}

// JSON 字符串字面量转义（只处理双引号/反斜杠/控制符，UTF-8 直通）
std::string jsonEscape(const std::string& in) {
  std::string out;
  out.reserve(in.size() + 8);
  for (char c : in) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          char buf[8];
          snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned char>(c));
          out += buf;
        } else {
          out.push_back(c);
        }
        break;
    }
  }
  return out;
}

std::string replaceAll(std::string text, const std::string& from, const std::string& to) {
  if (from.empty()) return text;
  std::string out;
  out.reserve(text.size());
  size_t pos = 0;
  while (pos < text.size()) {
    const size_t hit = text.find(from, pos);
    if (hit == std::string::npos) {
      out.append(text, pos, std::string::npos);
      break;
    }
    out.append(text, pos, hit - pos);
    out += to;
    pos = hit + from.size();
  }
  return out;
}

// ---------- 目录清理 ----------

bool removeDirInternal(const std::string& path) {
  if (path.empty()) return false;
#ifdef _WIN32
  return RemoveDirectoryW(utf8ToUtf16(path).c_str()) != 0;
#else
  return false;
#endif
}

bool removeTree(const std::string& path, std::string* err) {
  if (!dirExists(path)) return true;
  std::vector<DirEntry> entries;
  std::string listErr;
  if (!listDir(path, &entries, &listErr)) {
    if (err) *err = "读取目录失败（无法清理旧站点）: " + path + "（" + listErr + "）";
    return false;
  }
  for (const DirEntry& e : entries) {
    const std::string child = joinPath(path, e.name);
    if (e.isDir) {
      if (!removeTree(child, err)) return false;
    } else if (!removeFile(child)) {
      if (err) *err = "删除文件失败（无法清理旧站点）: " + child;
      return false;
    }
  }
  if (!removeDirInternal(path)) {
    if (err) *err = "删除目录失败（无法清理旧站点）: " + path;
    return false;
  }
  return true;
}

// ---------- SHA-256（FIPS 180-4；静态站前端用 Web Crypto 算同一个值比对） ----------

const uint32_t kSha256K[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u,
    0xab1c5ed5u, 0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu,
    0x9bdc06a7u, 0xc19bf174u, 0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu,
    0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau, 0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
    0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu,
    0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u, 0xa2bfe8a1u, 0xa81a664bu,
    0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u, 0x19a4c116u,
    0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u,
    0xc67178f2u};

inline uint32_t rotr(uint32_t x, uint32_t n) { return (x >> n) | (x << (32 - n)); }

void sha256Block(uint32_t h[8], const uint8_t block[64]) {
  uint32_t w[64];
  for (int i = 0; i < 16; ++i) {
    w[i] = (static_cast<uint32_t>(block[i * 4]) << 24) |
           (static_cast<uint32_t>(block[i * 4 + 1]) << 16) |
           (static_cast<uint32_t>(block[i * 4 + 2]) << 8) |
           static_cast<uint32_t>(block[i * 4 + 3]);
  }
  for (int i = 16; i < 64; ++i) {
    const uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
    const uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
  }
  uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
  for (int i = 0; i < 64; ++i) {
    const uint32_t s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
    const uint32_t ch = (e & f) ^ ((~e) & g);
    const uint32_t t1 = hh + s1 + ch + kSha256K[i] + w[i];
    const uint32_t s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
    const uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
    const uint32_t t2 = s0 + maj;
    hh = g;
    g = f;
    f = e;
    e = d + t1;
    d = c;
    c = b;
    b = a;
    a = t1 + t2;
  }
  h[0] += a;
  h[1] += b;
  h[2] += c;
  h[3] += d;
  h[4] += e;
  h[5] += f;
  h[6] += g;
  h[7] += hh;
}

std::string sha256HexOf(const std::string& text) {
  uint32_t h[8] = {0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
                   0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};
  const uint8_t* data = reinterpret_cast<const uint8_t*>(text.data());
  const uint64_t bitLen = static_cast<uint64_t>(text.size()) * 8u;
  size_t offset = 0;
  while (offset + 64 <= text.size()) {
    sha256Block(h, data + offset);
    offset += 64;
  }
  uint8_t tail[128];
  size_t tailLen = text.size() - offset;
  std::memset(tail, 0, sizeof(tail));
  if (tailLen) std::memcpy(tail, data + offset, tailLen);
  tail[tailLen] = 0x80;
  const size_t padded = (tailLen + 1 + 8 <= 64) ? 64 : 128;
  for (int i = 0; i < 8; ++i) {
    tail[padded - 1 - static_cast<size_t>(i)] =
        static_cast<uint8_t>((bitLen >> (8 * static_cast<uint64_t>(i))) & 0xFFu);
  }
  sha256Block(h, tail);
  if (padded == 128) sha256Block(h, tail + 64);

  static const char* kHex = "0123456789abcdef";
  std::string out;
  out.reserve(64);
  for (int i = 0; i < 8; ++i) {
    for (int shift = 28; shift >= 0; shift -= 4) {
      out.push_back(kHex[(h[i] >> shift) & 0xFu]);
    }
  }
  return out;
}

// ---------- 加密强度随机 ----------

// advapi32!SystemFunction036（即 RtlGenRandom）。用 GetProcAddress 动态取，避免额外链接依赖。
bool secureRandomBytes(uint8_t* out, size_t len) {
  if (!out || len == 0) return false;
#ifdef _WIN32
  typedef BOOLEAN(NTAPI * RtlGenRandomFn)(PVOID, ULONG);
  static RtlGenRandomFn fn = nullptr;
  static bool resolved = false;
  if (!resolved) {
    HMODULE advapi = GetModuleHandleW(L"advapi32.dll");
    if (!advapi) advapi = LoadLibraryW(L"advapi32.dll");
    if (advapi) {
      fn = reinterpret_cast<RtlGenRandomFn>(GetProcAddress(advapi, "SystemFunction036"));
    }
    resolved = true;
  }
  if (!fn) return false;
  return fn(out, static_cast<ULONG>(len)) != 0;
#else
  (void)out;
  (void)len;
  return false;
#endif
}

std::vector<int64_t> albumPhotoIds(db::AlbumDao* albums, int64_t albumId, std::string* err) {
  std::vector<int64_t> ids;
  if (!albums->listItems(albumId, &ids, err)) return std::vector<int64_t>();
  return ids;
}

}  // namespace

// ============================ 公开的哈希 / token ============================

std::string sha256Hex(const std::string& text) { return sha256HexOf(text); }

std::string secureToken(size_t hexChars) {
  const size_t bytes = (hexChars + 1) / 2;
  std::vector<uint8_t> buffer(bytes, 0);
  if (!secureRandomBytes(buffer.data(), buffer.size())) {
    // 退化路径：系统随机源不可用（不应发生）。用时间/计数器/地址熵混合后 SHA-256，
    // 仍然不可预测性有限 —— 记 warn，绝不假装"随机成功"。
    static uint64_t counter = 0;
    char mix[128];
    snprintf(mix, sizeof(mix), "fallback:%lld:%llu:%p:%s",
             static_cast<long long>(nowMs()), static_cast<unsigned long long>(++counter),
             static_cast<void*>(&counter), randomHex(32).c_str());
    logWarn("gallery", "secure random unavailable, token generated from fallback entropy");
    const std::string digest = sha256HexOf(mix);
    std::string out = digest;
    while (out.size() < hexChars) out += digest;
    out.resize(hexChars);
    return out;
  }
  static const char* kHex = "0123456789abcdef";
  std::string out;
  out.reserve(hexChars);
  for (size_t i = 0; i < hexChars; ++i) {
    const uint8_t byte = buffer[i / 2];
    out.push_back(kHex[(i % 2 == 0) ? (byte >> 4) : (byte & 0x0Fu)]);
  }
  return out;
}

// ============================ GalleryService ============================

GalleryService::GalleryService(core::Config* config, db::Database* db, db::PhotoDao* photos,
                               db::AlbumDao* albums, thumb::ThumbnailService* thumbs)
    : config_(config), db_(db), photos_(photos), albums_(albums), thumbs_(thumbs) {}

bool GalleryService::ensureTables(std::string* err) {
  if (!db_) {
    if (err) *err = "数据库未就绪";
    return false;
  }
  // 说明：schema.sql 由主理人独占（并行协作期间不改），这两张表用幂等 DDL 在首次使用时创建。
  // gallery_meta 存 title/customer_name/password_hash（albums 表没有这些列）；
  // gallery_feedback 存每个客户收藏状态（photos.rating/pick 是摄影师自己的筛选结果，不能覆盖）。
  static const char* kDdl =
      "CREATE TABLE IF NOT EXISTS gallery_meta("
      " album_id INTEGER PRIMARY KEY,"
      " token TEXT,"
      " title TEXT,"
      " customer_name TEXT,"
      " password_hash TEXT,"
      " created_at INT,"
      " updated_at INT);"
      "CREATE TABLE IF NOT EXISTS gallery_feedback("
      " album_id INT,"
      " photo_id INT,"
      " favorited INT DEFAULT 0,"
      " updated_at INT,"
      " PRIMARY KEY(album_id, photo_id));";
  std::string sqlErr;
  if (!db_->exec(kDdl, &sqlErr)) {
    if (err) *err = "初始化画廊表失败: " + sqlErr;
    return false;
  }
  return true;
}

bool GalleryService::preparePreview(int64_t photoId, std::string* previewPath, std::string* err) {
  if (!thumbs_) {
    if (err) *err = "缩略图服务未就绪";
    return false;
  }
  return thumbs_->ensureThumb(photoId, kGalleryImageSize, previewPath, err);
}

json::JsonValue GalleryService::publish(const json::JsonValue& params, rpc::RpcError* err) {
  if (!config_ || !db_ || !albums_ || !thumbs_) {
    if (err) *err = rpc::RpcError(rpc::kInternalError, "画廊服务依赖未就绪");
    return json::JsonValue::nullValue();
  }
  const int64_t albumId = params.get("albumId").asInt(0);
  if (albumId <= 0) {
    if (err) *err = rpc::RpcError(rpc::kInvalidParams, "albumId 非法: " + std::to_string(albumId));
    return json::JsonValue::nullValue();
  }

  AlbumRecord album;
  std::string dbErr;
  if (!albums_->getById(albumId, &album, &dbErr)) {
    if (err) {
      *err = rpc::RpcError(rpc::kFileNotFound, "画册不存在: " + std::to_string(albumId),
                           json::JsonValue::nullValue());
    }
    return json::JsonValue::nullValue();
  }

  const std::vector<int64_t> photoIds = albumPhotoIds(albums_, albumId, &dbErr);
  if (!dbErr.empty()) {
    if (err) *err = rpc::RpcError(rpc::kDbError, "读取画册成员失败: " + dbErr);
    return json::JsonValue::nullValue();
  }
  if (photoIds.empty()) {
    if (err) {
      *err = rpc::RpcError(rpc::kInvalidParams,
                           "画册为空，无法发布：先往画册里添加照片（album_items 无成员）");
    }
    return json::JsonValue::nullValue();
  }

  std::string tableErr;
  if (!ensureTables(&tableErr)) {
    if (err) *err = rpc::RpcError(rpc::kDbError, tableErr);
    return json::JsonValue::nullValue();
  }

  const std::string password = params.get("password").asString();
  const std::string title = trim(params.get("title").asString(album.name));
  const std::string customerName = trim(params.get("customerName").asString());
  int expiryDays = static_cast<int>(params.get("expiryDays").asInt(30));
  if (expiryDays < 1) expiryDays = 1;
  if (expiryDays > 3650) expiryDays = 3650;
  const int64_t publishedAt = nowMs();
  const int64_t expiresAt = publishedAt + static_cast<int64_t>(expiryDays) * 86400000LL;

  // 旧站点清理：同一画册重新发布必须替换旧 token（不许堆垃圾目录）
  const std::string galleryRoot = config_->galleryDir();
  if (!album.publishedToken.empty()) {
    const std::string oldDir = joinPath(galleryRoot, album.publishedToken);
    std::string cleanErr;
    if (!removeTree(oldDir, &cleanErr)) {
      if (err) *err = rpc::RpcError(rpc::kIoError, cleanErr);
      return json::JsonValue::nullValue();
    }
  }

  const std::string token = secureToken(48);
  const std::string staticDir = joinPath(galleryRoot, token);
  const std::string imagesDir = joinPath(staticDir, kImagesDir);
  if (!dirExists(staticDir) && !makeDirs(staticDir)) {
    if (err) *err = rpc::RpcError(rpc::kIoError, "创建画廊目录失败: " + staticDir);
    return json::JsonValue::nullValue();
  }
  if (!dirExists(imagesDir) && !makeDirs(imagesDir)) {
    if (err) *err = rpc::RpcError(rpc::kIoError, "创建画廊图片目录失败: " + imagesDir);
    return json::JsonValue::nullValue();
  }

  // 逐张准备预览图并复制进站点；单张失败进 missing（零静默），全部失败才整体报错
  std::vector<GalleryPhoto> photos;
  json::JsonValue missing = json::JsonValue::arrayValue();
  for (int64_t photoId : photoIds) {
    std::string previewPath;
    std::string thumbErr;
    if (!preparePreview(photoId, &previewPath, &thumbErr)) {
      json::JsonValue item = json::JsonValue::objectValue();
      item.setItem("photoId", photoId);
      item.setItem("error", thumbErr.empty() ? std::string("预览图生成失败") : thumbErr);
      missing.addItem(item);
      continue;
    }
    PhotoRecord record;
    std::string recordErr;
    int width = 0;
    int height = 0;
    std::string caption;
    if (photos_ && photos_->getById(photoId, &record, &recordErr)) {
      width = record.width;
      height = record.height;
      caption = baseNameNoExt(record.fileName.empty() ? record.filePath : record.fileName);
    }
    const std::string target = joinPath(imagesDir, std::to_string(photoId) + ".jpg");
    std::string copyErr;
    if (!copyFileTo(previewPath, target, &copyErr)) {
      json::JsonValue item = json::JsonValue::objectValue();
      item.setItem("photoId", photoId);
      item.setItem("error", "复制预览图失败: " + copyErr);
      missing.addItem(item);
      continue;
    }
    GalleryPhoto gp;
    gp.photoId = photoId;
    gp.width = width;
    gp.height = height;
    gp.caption = caption;
    photos.push_back(gp);
  }
  if (photos.empty()) {
    std::string cleanErr;
    removeTree(staticDir, &cleanErr);
    if (err) {
      *err = rpc::RpcError(rpc::kIoError, "画册内所有照片的预览图都无法生成，发布中止（详见 data.missing）",
                           [&missing]() {
                             json::JsonValue data = json::JsonValue::objectValue();
                             data.setItem("missing", missing);
                             return data;
                           }());
    }
    return json::JsonValue::nullValue();
  }

  const std::string passwordHash = sha256Hex(password + token);
  const std::string html =
      buildIndexHtml(albumId, title, customerName, photos, token, passwordHash, expiresAt);
  std::string writeErr;
  if (!writeFileAtomic(joinPath(staticDir, "index.html"), html, &writeErr)) {
    if (err) *err = rpc::RpcError(rpc::kIoError, "写入 index.html 失败: " + writeErr);
    return json::JsonValue::nullValue();
  }

  // meta.json：给 Electron 主进程拼 URL / 校验有效期用（不参与前端门禁）
  {
    json::JsonValue meta = json::JsonValue::objectValue();
    meta.setItem("albumId", albumId);
    meta.setItem("token", token);
    meta.setItem("title", title);
    meta.setItem("customerName", customerName);
    meta.setItem("passwordHash", passwordHash);
    meta.setItem("photoCount", static_cast<int64_t>(photos.size()));
    meta.setItem("publishedAt", publishedAt);
    meta.setItem("expiresAt", expiresAt);
    std::string metaErr;
    if (!writeFileAtomic(joinPath(staticDir, "meta.json"), meta.serialize(), &metaErr)) {
      if (err) *err = rpc::RpcError(rpc::kIoError, "写入 meta.json 失败: " + metaErr);
      return json::JsonValue::nullValue();
    }
  }

  // DB 登记：albums.published_* + gallery_meta（重启后 listPublished 仍可复原）
  if (!albums_->setPublished(albumId, token, publishedAt, expiresAt, &dbErr)) {
    if (err) *err = rpc::RpcError(rpc::kDbError, "登记发布信息失败: " + dbErr);
    return json::JsonValue::nullValue();
  }
  {
    std::string sqlErr;
    auto stmt = db_->prepareCached(
        "gallery.meta.upsert",
        "INSERT INTO gallery_meta(album_id,token,title,customer_name,password_hash,created_at,"
        "updated_at) VALUES(?,?,?,?,?,?,?) ON CONFLICT(album_id) DO UPDATE SET "
        "token=excluded.token,title=excluded.title,customer_name=excluded.customer_name,"
        "password_hash=excluded.password_hash,updated_at=excluded.updated_at;",
        &sqlErr);
    if (!stmt || !stmt->valid()) {
      if (err) *err = rpc::RpcError(rpc::kDbError, "写入画廊元信息失败: " + sqlErr);
      return json::JsonValue::nullValue();
    }
    stmt->bindInt64(1, albumId);
    stmt->bindText(2, token);
    stmt->bindText(3, title);
    stmt->bindText(4, customerName);
    stmt->bindText(5, passwordHash);
    stmt->bindInt64(6, publishedAt);
    stmt->bindInt64(7, publishedAt);
    const bool ok = stmt->step(&sqlErr);
    stmt->reset();
    if (!ok) {
      if (err) *err = rpc::RpcError(rpc::kDbError, "写入画廊元信息失败: " + sqlErr);
      return json::JsonValue::nullValue();
    }
  }

  logInfo("gallery", "published album " + std::to_string(albumId) + " → " + staticDir + " (" +
                         std::to_string(photos.size()) + " photos, token " + token + ")");

  json::JsonValue out = json::JsonValue::objectValue();
  out.setItem("token", token);
  out.setItem("staticDir", staticDir);
  out.setItem("expiresAt", expiresAt);
  // 协议层定位：由 Electron 主进程把 lumina-gallery://{token} 映射到 staticDir 的本地 http 服务；
  // 引擎不自造 localhost 端口（端口由主进程持有，编造 URL 等于假装能用）。
  out.setItem("previewUrl", "lumina-gallery://" + token);
  out.setItem("photos", static_cast<int64_t>(photos.size()));
  out.setItem("missing", missing);
  return out;
}

json::JsonValue GalleryService::revoke(const json::JsonValue& params, rpc::RpcError* err) {
  if (!config_ || !albums_) {
    if (err) *err = rpc::RpcError(rpc::kInternalError, "画廊服务依赖未就绪");
    return json::JsonValue::nullValue();
  }
  const int64_t albumId = params.get("albumId").asInt(0);
  if (albumId <= 0) {
    if (err) *err = rpc::RpcError(rpc::kInvalidParams, "albumId 非法: " + std::to_string(albumId));
    return json::JsonValue::nullValue();
  }
  AlbumRecord album;
  std::string dbErr;
  if (!albums_->getById(albumId, &album, &dbErr)) {
    if (err) *err = rpc::RpcError(rpc::kFileNotFound, "画册不存在: " + std::to_string(albumId));
    return json::JsonValue::nullValue();
  }
  if (album.publishedToken.empty()) {
    // 幂等：本来就没发布，直接成功（不是错误，也不静默丢信息 —— UI 只会看到 {}）
    return json::JsonValue::objectValue();
  }
  const std::string staticDir = joinPath(config_->galleryDir(), album.publishedToken);
  std::string cleanErr;
  if (!removeTree(staticDir, &cleanErr)) {
    // 目录删不干净也必须先把发布状态撤掉，否则 UI 会一直显示"已发布"却拿不到站点
    logWarn("gallery", "revoke cleanup failed: " + cleanErr);
    std::string clearErr;
    albums_->clearPublished(albumId, &clearErr);
    if (err) {
      *err = rpc::RpcError(rpc::kIoError, "静态站目录清理失败（发布状态已撤回）: " + cleanErr);
    }
    return json::JsonValue::nullValue();
  }
  if (!albums_->clearPublished(albumId, &dbErr)) {
    if (err) *err = rpc::RpcError(rpc::kDbError, "撤回发布状态失败: " + dbErr);
    return json::JsonValue::nullValue();
  }
  if (db_) {
    std::string sqlErr;
    auto stmt = db_->prepareCached("gallery.meta.delete", "DELETE FROM gallery_meta WHERE album_id=?;",
                                   &sqlErr);
    if (stmt && stmt->valid()) {
      stmt->bindInt64(1, albumId);
      if (!stmt->step(&sqlErr)) logWarn("gallery", "删除画廊元信息失败: " + sqlErr);
      stmt->reset();
    }
  }
  logInfo("gallery", "revoked album " + std::to_string(albumId) + " (dir " + staticDir + ")");
  return json::JsonValue::objectValue();
}

json::JsonValue GalleryService::feedbackStats(int64_t albumId, rpc::RpcError* err) {
  json::JsonValue stats = json::JsonValue::objectValue();
  int64_t total = 0;
  int64_t favorites = 0;
  int64_t marked = 0;
  if (!db_) {
    if (err) *err = rpc::RpcError(rpc::kInternalError, "数据库未就绪");
    return json::JsonValue::nullValue();
  }
  std::string sqlErr;
  {
    auto stmt = db_->prepareCached("gallery.stats.total",
                                   "SELECT COUNT(*) FROM album_items WHERE album_id=?;", &sqlErr);
    if (!stmt || !stmt->valid()) {
      if (err) *err = rpc::RpcError(rpc::kDbError, "统计画册张数失败: " + sqlErr);
      return json::JsonValue::nullValue();
    }
    stmt->bindInt64(1, albumId);
    if (stmt->step(&sqlErr)) total = stmt->columnInt64(0);
    stmt->reset();
  }
  {
    auto stmt = db_->prepareCached("gallery.stats.favorites",
                                   "SELECT COUNT(*) FROM gallery_feedback WHERE album_id=?;",
                                   &sqlErr);
    if (!stmt || !stmt->valid()) {
      if (err) *err = rpc::RpcError(rpc::kDbError, "统计收藏数失败: " + sqlErr);
      return json::JsonValue::nullValue();
    }
    stmt->bindInt64(1, albumId);
    if (stmt->step(&sqlErr)) marked = stmt->columnInt64(0);
    stmt->reset();
  }
  {
    auto stmt = db_->prepareCached("gallery.stats.favTrue",
                                   "SELECT COUNT(*) FROM gallery_feedback WHERE album_id=? AND "
                                   "favorited=1;",
                                   &sqlErr);
    if (!stmt || !stmt->valid()) {
      if (err) *err = rpc::RpcError(rpc::kDbError, "统计收藏数失败: " + sqlErr);
      return json::JsonValue::nullValue();
    }
    stmt->bindInt64(1, albumId);
    if (stmt->step(&sqlErr)) favorites = stmt->columnInt64(0);
    stmt->reset();
  }
  stats.setItem("photos", total);
  stats.setItem("favorites", favorites);
  stats.setItem("completionRate", total > 0 ? static_cast<double>(marked) / static_cast<double>(total)
                                            : 0.0);
  return stats;
}

json::JsonValue GalleryService::listPublished(const json::JsonValue& params, rpc::RpcError* err) {
  (void)params;
  if (!config_ || !albums_ || !db_) {
    if (err) *err = rpc::RpcError(rpc::kInternalError, "画廊服务依赖未就绪");
    return json::JsonValue::nullValue();
  }
  std::vector<AlbumRecord> records;
  std::string dbErr;
  if (!albums_->listPublished(&records, &dbErr)) {
    if (err) *err = rpc::RpcError(rpc::kDbError, "读取已发布列表失败: " + dbErr);
    return json::JsonValue::nullValue();
  }
  // 辅助表可能还没建（例如主理人在 schema 并入前跑过一次老版本），先幂等补齐再统计
  std::string tableErr;
  if (!ensureTables(&tableErr)) {
    if (err) *err = rpc::RpcError(rpc::kDbError, tableErr);
    return json::JsonValue::nullValue();
  }
  const int64_t now = nowMs();
  json::JsonValue out = json::JsonValue::arrayValue();
  for (const AlbumRecord& album : records) {
    rpc::RpcError statsErr;
    json::JsonValue stats = feedbackStats(album.id, &statsErr);
    if (stats.isNull()) {
      if (err) *err = statsErr;
      return json::JsonValue::nullValue();
    }
    json::JsonValue item = json::JsonValue::objectValue();
    item.setItem("albumId", album.id);
    item.setItem("token", album.publishedToken);
    item.setItem("previewUrl", "lumina-gallery://" + album.publishedToken);
    item.setItem("staticDir", joinPath(config_->galleryDir(), album.publishedToken));
    item.setItem("name", album.name);
    item.setItem("expiresAt", album.hasExpiresAt ? json::JsonValue::integer(album.expiresAt)
                                                 : json::JsonValue::nullValue());
    item.setItem("expired", album.hasExpiresAt && album.expiresAt > 0 && now > album.expiresAt);
    item.setItem("stats", stats);
    out.addItem(item);
  }
  return out;
}

json::JsonValue GalleryService::importFeedback(const json::JsonValue& params, rpc::RpcError* err) {
  if (!db_ || !albums_) {
    if (err) *err = rpc::RpcError(rpc::kInternalError, "画廊服务依赖未就绪");
    return json::JsonValue::nullValue();
  }
  const int64_t albumId = params.get("albumId").asInt(0);
  if (albumId <= 0) {
    if (err) *err = rpc::RpcError(rpc::kInvalidParams, "albumId 非法: " + std::to_string(albumId));
    return json::JsonValue::nullValue();
  }
  const std::string filePath = params.get("filePath").asString();
  if (filePath.empty()) {
    if (err) *err = rpc::RpcError(rpc::kInvalidParams, "filePath 不能为空");
    return json::JsonValue::nullValue();
  }
  if (!fileExists(filePath)) {
    if (err) *err = rpc::RpcError(rpc::kFileNotFound, "收藏包不存在: " + filePath);
    return json::JsonValue::nullValue();
  }
  std::string bytes;
  std::string readErr;
  if (!readFile(filePath, &bytes, &readErr)) {
    if (err) *err = rpc::RpcError(rpc::kIoError, "读取收藏包失败: " + readErr);
    return json::JsonValue::nullValue();
  }
  // 收藏包结构（由静态站 index.html 导出）：
  // { "schema":"lumina.gallery.feedback/1", "albumId":<n>, "token":"<token>",
  //   "exportedAt":<epochMs>, "photos":[{"photoId":<n>,"favorited":<bool>}, ...] }
  // 兼容简写：{ "favorites": [photoId, ...] }（等价于这些 photoId favorited=true）
  json::JsonValue doc;
  std::string parseErr;
  if (!json::JsonValue::parse(bytes, &doc, &parseErr)) {
    if (err) *err = rpc::RpcError(rpc::kInvalidParams, "收藏包不是合法 JSON: " + parseErr);
    return json::JsonValue::nullValue();
  }
  if (!doc.isObject()) {
    if (err) *err = rpc::RpcError(rpc::kInvalidParams, "收藏包顶层必须是对象");
    return json::JsonValue::nullValue();
  }
  if (doc.contains("albumId") && doc.get("albumId").asInt(0) != albumId) {
    if (err) {
      *err = rpc::RpcError(rpc::kInvalidParams,
                           "收藏包属于画册 " + std::to_string(doc.get("albumId").asInt(0)) +
                               "，与请求的 albumId " + std::to_string(albumId) + " 不一致");
    }
    return json::JsonValue::nullValue();
  }

  std::string dbErr;
  const std::vector<int64_t> albumIds = albumPhotoIds(albums_, albumId, &dbErr);
  if (!dbErr.empty()) {
    if (err) *err = rpc::RpcError(rpc::kDbError, "读取画册成员失败: " + dbErr);
    return json::JsonValue::nullValue();
  }
  std::vector<int64_t> members(albumIds.begin(), albumIds.end());

  struct Entry {
    int64_t photoId;
    bool favorited;
  };
  std::vector<Entry> entries;
  int64_t ignored = 0;
  if (doc.get("photos").isArray()) {
    for (const json::JsonValue& item : doc.get("photos").arrayItems()) {
      const int64_t photoId = item.get("photoId").asInt(0);
      if (photoId <= 0) {
        ++ignored;
        continue;
      }
      if (std::find(members.begin(), members.end(), photoId) == members.end()) {
        ++ignored;  // 不属于本画册的条目：计数上报，不静默塞库
        continue;
      }
      entries.push_back(Entry{photoId, item.get("favorited").asBool(false)});
    }
  } else if (doc.get("favorites").isArray()) {
    for (const json::JsonValue& item : doc.get("favorites").arrayItems()) {
      const int64_t photoId = item.asInt(0);
      if (photoId <= 0 || std::find(members.begin(), members.end(), photoId) == members.end()) {
        ++ignored;
        continue;
      }
      entries.push_back(Entry{photoId, true});
    }
  } else {
    if (err) {
      *err = rpc::RpcError(rpc::kInvalidParams,
                           "收藏包缺少 photos 数组（或兼容的 favorites 数组）");
    }
    return json::JsonValue::nullValue();
  }
  if (entries.empty()) {
    if (err) {
      *err = rpc::RpcError(rpc::kInvalidParams, "收藏包内没有属于该画册的条目（ignored=" +
                                                    std::to_string(ignored) + "）");
    }
    return json::JsonValue::nullValue();
  }

  std::string tableErr;
  if (!ensureTables(&tableErr)) {
    if (err) *err = rpc::RpcError(rpc::kDbError, tableErr);
    return json::JsonValue::nullValue();
  }

  const int64_t now = nowMs();
  int64_t favorites = 0;
  if (!db_->begin(&dbErr)) {
    if (err) *err = rpc::RpcError(rpc::kDbError, "开启事务失败: " + dbErr);
    return json::JsonValue::nullValue();
  }
  {
    std::string sqlErr;
    auto stmt = db_->prepareCached(
        "gallery.feedback.upsert",
        "INSERT INTO gallery_feedback(album_id,photo_id,favorited,updated_at) VALUES(?,?,?,?) "
        "ON CONFLICT(album_id,photo_id) DO UPDATE SET favorited=excluded.favorited, "
        "updated_at=excluded.updated_at;",
        &sqlErr);
    if (!stmt || !stmt->valid()) {
      db_->rollback(&dbErr);
      if (err) *err = rpc::RpcError(rpc::kDbError, "写入收藏反馈失败: " + sqlErr);
      return json::JsonValue::nullValue();
    }
    for (const Entry& entry : entries) {
      stmt->bindInt64(1, albumId);
      stmt->bindInt64(2, entry.photoId);
      stmt->bindInt(3, entry.favorited ? 1 : 0);
      stmt->bindInt64(4, now);
      if (!stmt->step(&sqlErr)) {
        stmt->reset();
        db_->rollback(&dbErr);
        if (err) *err = rpc::RpcError(rpc::kDbError, "写入收藏反馈失败: " + sqlErr);
        return json::JsonValue::nullValue();
      }
      stmt->reset();
      if (entry.favorited) ++favorites;
    }
  }
  if (!db_->commit(&dbErr)) {
    if (err) *err = rpc::RpcError(rpc::kDbError, "提交收藏反馈失败: " + dbErr);
    return json::JsonValue::nullValue();
  }

  const double completionRate =
      albumIds.empty() ? 0.0 : static_cast<double>(entries.size()) / static_cast<double>(albumIds.size());
  logInfo("gallery", "imported feedback for album " + std::to_string(albumId) + ": entries=" +
                         std::to_string(entries.size()) + " favorites=" + std::to_string(favorites) +
                         " ignored=" + std::to_string(ignored));

  json::JsonValue out = json::JsonValue::objectValue();
  out.setItem("favorites", favorites);
  out.setItem("completionRate", completionRate);
  out.setItem("imported", static_cast<int64_t>(entries.size()));
  out.setItem("ignored", ignored);
  return out;
}

json::JsonValue GalleryService::getFeedback(const json::JsonValue& params, rpc::RpcError* err) {
  if (!db_ || !albums_) {
    if (err) *err = rpc::RpcError(rpc::kInternalError, "画廊服务依赖未就绪");
    return json::JsonValue::nullValue();
  }
  const int64_t albumId = params.get("albumId").asInt(0);
  if (albumId <= 0) {
    if (err) *err = rpc::RpcError(rpc::kInvalidParams, "albumId 非法: " + std::to_string(albumId));
    return json::JsonValue::nullValue();
  }
  std::string tableErr;
  if (!ensureTables(&tableErr)) {
    if (err) *err = rpc::RpcError(rpc::kDbError, tableErr);
    return json::JsonValue::nullValue();
  }
  rpc::RpcError statsErr;
  json::JsonValue stats = feedbackStats(albumId, &statsErr);
  if (stats.isNull()) {
    if (err) *err = statsErr;
    return json::JsonValue::nullValue();
  }

  // 画册成员（按 position 顺序）+ 每张的 favorited 状态
  std::vector<int64_t> members;
  std::string dbErr;
  if (!albums_->listItems(albumId, &members, &dbErr)) {
    if (err) *err = rpc::RpcError(rpc::kDbError, "读取画册成员失败: " + dbErr);
    return json::JsonValue::nullValue();
  }
  std::vector<int64_t> favoritedIds;
  {
    std::string sqlErr;
    auto stmt = db_->prepareCached("gallery.feedback.list",
                                   "SELECT photo_id FROM gallery_feedback WHERE album_id=? AND "
                                   "favorited=1;",
                                   &sqlErr);
    if (!stmt || !stmt->valid()) {
      if (err) *err = rpc::RpcError(rpc::kDbError, "读取收藏反馈失败: " + sqlErr);
      return json::JsonValue::nullValue();
    }
    stmt->bindInt64(1, albumId);
    std::string stepErr;
    while (stmt->step(&stepErr)) favoritedIds.push_back(stmt->columnInt64(0));
    stmt->reset();
    if (!stepErr.empty()) {
      if (err) *err = rpc::RpcError(rpc::kDbError, "读取收藏反馈失败: " + stepErr);
      return json::JsonValue::nullValue();
    }
  }

  json::JsonValue perPhoto = json::JsonValue::arrayValue();
  for (int64_t photoId : members) {
    json::JsonValue item = json::JsonValue::objectValue();
    item.setItem("photoId", photoId);
    item.setItem("favorited",
                 std::find(favoritedIds.begin(), favoritedIds.end(), photoId) != favoritedIds.end());
    perPhoto.addItem(item);
  }

  json::JsonValue out = json::JsonValue::objectValue();
  out.setItem("favoritesCount", stats.get("favorites").asInt(0));
  out.setItem("completionRate", stats.get("completionRate").asDouble(0.0));
  out.setItem("photos", stats.get("photos").asInt(0));
  out.setItem("perPhoto", perPhoto);
  return out;
}

// ============================ 静态站模板 ============================

namespace {

// 深色 Glass Noir：背景 #0C0D10 / 卡片 #1D2025 / 点缀 #E8C268 / 照片区 #08090B
const char* kHtmlTemplate = R"HTML(<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>__TITLE__ · LUMINA 客户画廊</title>
<style>
  :root { --bg:#0C0D10; --card:#1D2025; --accent:#E8C268; --photo:#08090B; --text:#E7E9EE; --dim:#8C929E; }
  * { box-sizing: border-box; }
  body { margin:0; background:var(--bg); color:var(--text);
         font-family: -apple-system, "Segoe UI", "PingFang SC", "Microsoft YaHei", sans-serif; }
  header { padding:28px 32px 18px; border-bottom:1px solid #22252B; }
  h1 { margin:0 0 6px; font-size:22px; font-weight:600; letter-spacing:.02em; }
  .meta { color:var(--dim); font-size:13px; }
  .meta b { color:var(--accent); font-weight:600; }
  main { padding:24px 32px 64px; }
  .grid { display:grid; gap:14px; grid-template-columns:repeat(auto-fill,minmax(240px,1fr)); }
  figure { margin:0; background:var(--card); border-radius:10px; overflow:hidden;
           border:1px solid #24272E; position:relative; }
  .shot { background:var(--photo); display:block; width:100%; height:180px; object-fit:cover;
          cursor:zoom-in; }
  figcaption { padding:8px 10px; font-size:12px; color:var(--dim); display:flex;
               justify-content:space-between; align-items:center; gap:8px; }
  .fav { background:none; border:1px solid #2E323A; color:var(--dim); border-radius:999px;
         padding:3px 10px; font-size:12px; cursor:pointer; }
  .fav[aria-pressed="true"] { color:#0C0D10; background:var(--accent); border-color:var(--accent); }
  .bar { position:fixed; right:24px; bottom:24px; display:flex; gap:10px; align-items:center; }
  button.primary { background:var(--accent); color:#0C0D10; border:none; border-radius:8px;
                   padding:10px 16px; font-weight:600; cursor:pointer; }
  button.ghost { background:transparent; color:var(--text); border:1px solid #2E323A;
                 border-radius:8px; padding:10px 16px; cursor:pointer; }
  .gate { position:fixed; inset:0; background:rgba(8,9,11,.96); display:flex; align-items:center;
          justify-content:center; z-index:50; }
  .gate .box { background:var(--card); border:1px solid #262A31; border-radius:12px; padding:26px;
               width:340px; }
  .gate h2 { margin:0 0 6px; font-size:17px; }
  .gate p { margin:0 0 16px; color:var(--dim); font-size:12px; line-height:1.6; }
  .gate input { width:100%; padding:10px 12px; border-radius:8px; border:1px solid #2E323A;
                background:#101216; color:var(--text); font-size:14px; }
  .gate .row { display:flex; gap:10px; margin-top:14px; }
  .gate .err { color:#F06A6A; font-size:12px; margin-top:10px; min-height:16px; }
  .lightbox { position:fixed; inset:0; background:rgba(4,5,7,.96); display:none;
              align-items:center; justify-content:center; z-index:60; }
  .lightbox img { max-width:92vw; max-height:86vh; box-shadow:0 20px 60px rgba(0,0,0,.6); }
  .lightbox .close { position:absolute; top:18px; right:22px; background:none; border:none;
                     color:var(--text); font-size:26px; cursor:pointer; }
  footer { color:#5C626D; font-size:12px; padding:0 32px 40px; }
  .hidden { display:none !important; }
</style>
</head>
<body>
<div class="gate" id="gate">
  <form class="box" id="gateForm" autocomplete="off">
    <h2>__TITLE__</h2>
    <p>这是 __CUSTOMER__ 的专属画廊（__COUNT__ 张）。请输入查看密码。<br>
       本页门禁为<b>本地预览级</b>，密码校验在浏览器内完成，不是服务端鉴权。</p>
    <input id="pw" type="password" placeholder="查看密码" required>
    <div class="row">
      <button class="primary" type="submit">进入画廊</button>
    </div>
    <div class="err" id="gateErr"></div>
  </form>
</div>

<header>
  <h1>__TITLE__</h1>
  <div class="meta">客户 <b>__CUSTOMER__</b> · 共 <b>__COUNT__</b> 张 · 有效期至 <b>__EXPIRES__</b></div>
</header>

<main>
  <div class="grid" id="grid"></div>
</main>

<footer>由 LUMINA 导出 · 收藏结果会被记录在本机浏览器，点击右下角「导出收藏包」交给摄影师。</footer>

<div class="bar">
  <button class="ghost" id="clearBtn">清除收藏</button>
  <button class="primary" id="exportBtn">导出收藏包</button>
</div>

<div class="lightbox" id="lightbox">
  <button class="close" id="closeBtn">&times;</button>
  <img id="lightboxImg" alt="">
</div>

<script>
const ALBUM_ID = __ALBUM_ID__;
const TOKEN = "__TOKEN__";
const EXPECTED = "__PASSWORD_HASH__";
const PHOTOS = __PHOTOS__;
const FAV_KEY = 'lumina-fav-' + TOKEN;
const GATE_KEY = 'lumina-gate-' + TOKEN;

function loadFavs() {
  try { return JSON.parse(localStorage.getItem(FAV_KEY) || '{}'); } catch (e) { return {}; }
}
function saveFavs(favs) { localStorage.setItem(FAV_KEY, JSON.stringify(favs)); }

function hexOf(buffer) {
  return Array.from(new Uint8Array(buffer)).map(b => b.toString(16).padStart(2, '0')).join('');
}

async function sha256Hex(text) {
  if (!window.crypto || !window.crypto.subtle) {
    throw new Error('当前环境不支持 Web Crypto（请用 LUMINA 客户端或本地 http 打开本页）');
  }
  const data = new TextEncoder().encode(text);
  return hexOf(await window.crypto.subtle.digest('SHA-256', data));
}

function renderGrid() {
  const favs = loadFavs();
  const grid = document.getElementById('grid');
  grid.innerHTML = '';
  PHOTOS.forEach(p => {
    const fig = document.createElement('figure');
    const img = document.createElement('img');
    img.className = 'shot';
    img.src = 'images/' + p.id + '.jpg';
    img.alt = p.caption || ('照片 ' + p.id);
    img.loading = 'lazy';
    img.addEventListener('click', () => openLightbox(p));
    const cap = document.createElement('figcaption');
    const label = document.createElement('span');
    label.textContent = p.caption || ('#' + p.id);
    const fav = document.createElement('button');
    fav.className = 'fav';
    fav.type = 'button';
    fav.setAttribute('aria-pressed', favs[p.id] ? 'true' : 'false');
    fav.textContent = favs[p.id] ? '已收藏' : '收藏';
    fav.addEventListener('click', () => {
      const cur = loadFavs();
      cur[p.id] = !cur[p.id];
      saveFavs(cur);
      fav.setAttribute('aria-pressed', cur[p.id] ? 'true' : 'false');
      fav.textContent = cur[p.id] ? '已收藏' : '收藏';
    });
    cap.appendChild(label);
    cap.appendChild(fav);
    fig.appendChild(img);
    fig.appendChild(cap);
    grid.appendChild(fig);
  });
}

function openLightbox(p) {
  const lb = document.getElementById('lightbox');
  document.getElementById('lightboxImg').src = 'images/' + p.id + '.jpg';
  lb.style.display = 'flex';
}
document.getElementById('closeBtn').addEventListener('click', () => {
  document.getElementById('lightbox').style.display = 'none';
});
document.getElementById('lightbox').addEventListener('click', (ev) => {
  if (ev.target.id === 'lightbox') ev.target.style.display = 'none';
});

document.getElementById('clearBtn').addEventListener('click', () => {
  if (!confirm('清除本机所有收藏标记？')) return;
  localStorage.removeItem(FAV_KEY);
  renderGrid();
});

document.getElementById('exportBtn').addEventListener('click', () => {
  const favs = loadFavs();
  const photos = PHOTOS.map(p => ({ photoId: p.id, favorited: !!favs[p.id] }));
  const pack = {
    schema: 'lumina.gallery.feedback/1',
    albumId: ALBUM_ID,
    token: TOKEN,
    exportedAt: Date.now(),
    photos: photos
  };
  const blob = new Blob([JSON.stringify(pack, null, 2)], { type: 'application/json' });
  const url = URL.createObjectURL(blob);
  const a = document.createElement('a');
  a.href = url;
  a.download = 'feedback-' + TOKEN.slice(0, 8) + '.json';
  a.click();
  URL.revokeObjectURL(url);
});

const gate = document.getElementById('gate');
async function unlock() {
  gate.classList.add('hidden');
  sessionStorage.setItem(GATE_KEY, '1');
  renderGrid();
}

document.getElementById('gateForm').addEventListener('submit', async (ev) => {
  ev.preventDefault();
  const errBox = document.getElementById('gateErr');
  errBox.textContent = '';
  const pw = document.getElementById('pw').value;
  try {
    const got = await sha256Hex(pw + TOKEN);
    if (got === EXPECTED) {
      await unlock();
    } else {
      errBox.textContent = '密码不正确';
    }
  } catch (e) {
    errBox.textContent = e.message || String(e);
  }
});

if (sessionStorage.getItem(GATE_KEY) === '1') {
  unlock();
}
</script>
</body>
</html>
)HTML";

}  // namespace

std::string buildIndexHtml(int64_t albumId, const std::string& title,
                           const std::string& customerName,
                           const std::vector<GalleryPhoto>& photos, const std::string& token,
                           const std::string& passwordHash, int64_t expiresAtMs) {
  std::string photosJson = "[";
  for (size_t i = 0; i < photos.size(); ++i) {
    if (i) photosJson += ",";
    const GalleryPhoto& p = photos[i];
    photosJson += "{\"id\":" + std::to_string(p.photoId);
    photosJson += ",\"w\":" + std::to_string(p.width);
    photosJson += ",\"h\":" + std::to_string(p.height);
    photosJson += ",\"caption\":\"" + jsonEscape(p.caption) + "\"}";
  }
  photosJson += "]";

  const std::string customer = customerName.empty() ? std::string("尊贵的客户") : customerName;
  std::string html = kHtmlTemplate;
  html = replaceAll(html, "__TITLE__", htmlEscape(title.empty() ? std::string("客户画廊") : title));
  html = replaceAll(html, "__CUSTOMER__", htmlEscape(customer));
  html = replaceAll(html, "__COUNT__", std::to_string(photos.size()));
  html = replaceAll(html, "__EXPIRES__",
                    expiresAtMs > 0 ? htmlEscape(formatDate(expiresAtMs)) : std::string("长期有效"));
  html = replaceAll(html, "__ALBUM_ID__", std::to_string(albumId));
  html = replaceAll(html, "__TOKEN__", token);
  html = replaceAll(html, "__PASSWORD_HASH__", passwordHash);
  html = replaceAll(html, "__PHOTOS__", photosJson);
  return html;
}

}  // namespace gallery
}  // namespace lumina
