// 缩略图服务实现：RAW 优先取内嵌代理，否则 LibRaw 真解码；落盘后发事件
#include "thumb/thumbnail_service.h"

#include <atomic>

#include "core/logger.h"
#include "core/str_util.h"
#include "raw/embedded_proxy.h"
#include "raw/raw_decoder.h"

namespace lumina {
namespace thumb {

ThumbnailService::ThumbnailService(core::Config* config, db::PhotoDao* photos,
                                   core::ThreadPool* pool, ReadyFn onReady)
    : config_(config), photos_(photos), pool_(pool), onReady_(std::move(onReady)) {}

std::string ThumbnailService::cachePath(int64_t photoId, int size) const {
  return joinPath(config_->thumbsDir(), std::to_string(size) + "/" + std::to_string(photoId) + ".jpg");
}

int ThumbnailService::scaledHeight(int width, int height, int targetWidth) {
  if (width <= 0 || height <= 0) return targetWidth;
  int h = targetWidth * height / width;
  return h <= 0 ? targetWidth : h;
}

bool ThumbnailService::ensureThumb(int64_t photoId, int size, std::string* outPath,
                                   std::string* err) {
  std::string target = cachePath(photoId, size);
  if (fileExists(target) && fileSize(target) > 0) {
    *outPath = target;
    return true;
  }
  PhotoRecord p;
  std::string dbErr;
  if (!photos_->getById(photoId, &p, &dbErr)) {
    if (err) *err = "photo not found: " + std::to_string(photoId);
    return false;
  }
  if (!fileExists(p.filePath)) {
    if (err) *err = "original file missing: " + p.filePath;
    return false;
  }
  std::string ext = extensionOf(p.filePath);
  bool ok = false;
  // 必须用 isRawExtension 判定：isSupportedExtension 含 JPG/PNG/TIFF 等标准格式，
  // 拿它当"是 RAW"会让标准格式进 LibRaw 分支并必然失败
  // （实测 PNG/JPG 缩略图全报 "libraw open failed (-2)"，图库永远无缩略图）。
  if (raw::isRawExtension(ext)) {
    // 1) 内嵌 JPEG 代理（RW2/DNG 都有）——最快
    raw::EmbeddedProxy proxy;
    std::string tmp = joinPath(config_->thumbsDir(),
                               "tmp/proxy-" + std::to_string(photoId) + "-" + randomHex(6) + ".jpg");
    if (raw::extractEmbeddedJpeg(p.filePath, tmp, &proxy, nullptr) && fileExists(tmp)) {
      imaging::Image image;
      std::string decodeErr;
      if (imaging::decodeImageFile(tmp, &image, &decodeErr)) {
        imaging::Image scaled;
        imaging::resizeImage(image, &scaled, size, scaledHeight(image.width, image.height, size));
        if (imaging::encodeJpeg(target, scaled, 82, err)) ok = true;
      }
      removeFile(tmp);
    }
    // 2) 回退：LibRaw 真解码（红线：不伪造预览）
    if (!ok) {
      imaging::Image image;
      std::string decodeErr;
      if (raw::RawDecoder().decodeToBitmap(p.filePath, true, &image, &decodeErr)) {
        imaging::Image scaled;
        imaging::resizeImage(image, &scaled, size, scaledHeight(image.width, image.height, size));
        if (imaging::encodeJpeg(target, scaled, 82, err)) ok = true;
      } else {
        *err = decodeErr;
      }
    }
  } else {
    imaging::Image image;
    std::string decodeErr;
    if (imaging::decodeImageFile(p.filePath, &image, &decodeErr)) {
      imaging::Image scaled;
      imaging::resizeImage(image, &scaled, size, scaledHeight(image.width, image.height, size));
      if (imaging::encodeJpeg(target, scaled, 82, err)) ok = true;
    } else {
      *err = decodeErr;
    }
  }
  if (!ok) {
    logWarn("thumb", "thumb failed for photo " + std::to_string(photoId) + ": " + *err);
    return false;
  }
  *outPath = target;
  return true;
}

void ThumbnailService::generateAsync(int64_t photoId, int size) {
  const ThumbKey key(photoId, size);
  {
    std::lock_guard<std::mutex> lock(mutex_);
    // 键含 size：同一张照片的 320 与 640 是两条独立任务，互不吞并。
    if (inFlight_.find(key) != inFlight_.end()) return;
    inFlight_[key] = true;
  }
  pool_->submit([this, photoId, size, key]() {
    std::string path;
    std::string err;
    bool ok = ensureThumb(photoId, size, &path, &err);
    {
      std::lock_guard<std::mutex> lock(mutex_);
      // 无论成功失败都必须清 inFlight_，否则一次失败会让该 (photo,size) 永不重试。
      inFlight_.erase(key);
      if (ok) {
        failures_.erase(key);
      } else {
        failures_[key] = err;
      }
    }
    if (ok && onReady_) onReady_(photoId, size, path);
    else logWarn("thumb", "async thumb failed for " + std::to_string(photoId) + " @" +
                              std::to_string(size) + ": " + err);
  });
}

json::JsonValue ThumbnailService::ensureBatch(const std::vector<int64_t>& ids, int size,
                                              rpc::RpcError* err) {
  if (size != 320 && size != 640) {
    *err = rpc::RpcError(rpc::kInvalidParams, "params.size must be 320 or 640");
    return json::JsonValue::nullValue();
  }
  json::JsonValue items = json::JsonValue::arrayValue();
  for (int64_t id : ids) {
    json::JsonValue item = json::JsonValue::objectValue();
    item.setItem("id", id);
    std::string target = cachePath(id, size);
    if (fileExists(target) && fileSize(target) > 0) {
      item.setItem("state", "ready");
      item.setItem("path", target);
      items.addItem(item);
      continue;
    }
    // 协议 §5.3：缺图返回 pending 并后台生成，就绪后逐张发 event.thumb.ready。
    // 不许同步逐张解码：5 万张库冷首屏实测会阻塞 14.7s（200 张 × 73ms/张），
    // UI 全程卡死；异步后本次调用毫秒级返回，UI 按 ready 事件逐步补图。
    item.setItem("state", "pending");
    items.addItem(item);
    generateAsync(id, size);
  }
  json::JsonValue out = json::JsonValue::objectValue();
  out.setItem("items", items);
  return out;
}

json::JsonValue ThumbnailService::getStatus(const std::vector<int64_t>& ids, int size,
                                            rpc::RpcError* err) {
  if (size != 320 && size != 640) {
    *err = rpc::RpcError(rpc::kInvalidParams, "params.size must be 320 or 640");
    return json::JsonValue::nullValue();
  }
  json::JsonValue items = json::JsonValue::arrayValue();
  for (int64_t id : ids) {
    json::JsonValue item = json::JsonValue::objectValue();
    item.setItem("id", id);
    std::string target = cachePath(id, size);
    if (fileExists(target) && fileSize(target) > 0) {
      item.setItem("state", "ready");
      item.setItem("path", target);
    } else {
      // 失败要显式呈现：否则 UI 见 pending 永远转圈，用户没有任何线索
      const ThumbKey key(id, size);
      std::string failure;
      {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = failures_.find(key);
        if (it != failures_.end()) failure = it->second;
      }
      if (!failure.empty()) {
        item.setItem("state", "failed");
        item.setItem("error", failure);
      } else {
        item.setItem("state", "pending");
      }
    }
    items.addItem(item);
  }
  json::JsonValue out = json::JsonValue::objectValue();
  out.setItem("items", items);
  return out;
}

}  // namespace thumb
}  // namespace lumina
