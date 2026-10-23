// 缩略图服务：ensure/getStatus、缓存落盘、event.thumb.ready 回调
#ifndef LUMINA_THUMB_THUMBNAIL_SERVICE_H_
#define LUMINA_THUMB_THUMBNAIL_SERVICE_H_

#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "core/config.h"
#include "core/thread_pool.h"
#include "db/photo_dao.h"
#include "json/json_value.h"
#include "rpc/dispatcher.h"

namespace lumina {
namespace thumb {

class ThumbnailService {
 public:
  using ReadyFn = std::function<void(int64_t photoId, int size, const std::string& path)>;

  ThumbnailService(core::Config* config, db::PhotoDao* photos, core::ThreadPool* pool,
                   ReadyFn onReady);

  // 生成（或复用）缩略图；失败返回 false 并在 items 中给出 failed
  json::JsonValue ensureBatch(const std::vector<int64_t>& ids, int size, rpc::RpcError* err);
  json::JsonValue getStatus(const std::vector<int64_t>& ids, int size, rpc::RpcError* err);

  // 单张：同步生成；outPath 为绝对路径
  bool ensureThumb(int64_t photoId, int size, std::string* outPath, std::string* err);

  std::string cachePath(int64_t photoId, int size) const;

 private:
  void generateAsync(int64_t photoId, int size);
  static int scaledHeight(int width, int height, int targetWidth);

  core::Config* config_;
  db::PhotoDao* photos_;
  core::ThreadPool* pool_;
  ReadyFn onReady_;
  mutable std::mutex mutex_;
  std::map<int64_t, bool> inFlight_;
};

}  // namespace thumb
}  // namespace lumina

#endif  // LUMINA_THUMB_THUMBNAIL_SERVICE_H_
