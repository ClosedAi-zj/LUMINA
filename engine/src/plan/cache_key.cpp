#include "plan/cache_key.h"

#include <algorithm>
#include <map>

#include "core/md5.h"

namespace lumina {
namespace plan {

namespace {

bool isOneOf(const std::string& path, std::initializer_list<const char*> names) {
  for (const char* n : names) {
    if (path == n) return true;
  }
  return false;
}

}  // namespace

CacheBucket bucketForParam(const std::string& path) {
  // kGeom：任何重映射像素位置的参数（含镜头畸变——畸变属于 S1 几何输出）
  if (path.rfind("geometry.", 0) == 0 || path.rfind("lens.", 0) == 0 ||
      isOneOf(path, {"optics.manualDistortion", "optics.lensProfileEnable"})) {
    return CacheBucket::kGeom;
  }
  // kTone：tone map 之前线性光算子（lead 口径：basic/calibration/detail(NR)/icc）
  if (isOneOf(path,
              {"basic.exposure", "basic.whites", "basic.blacks",
               "basic.contrast", "basic.highlights", "basic.shadows",
               "basic.temperature", "basic.tint", "basic.whiteBalance",
               "icc.profile", "icc.renderIntent"}) ||
      path.rfind("calibration.", 0) == 0 ||
      isOneOf(path, {"detail.colorNR", "detail.colorNRDetail",
                     "detail.luminanceSmoothing"})) {
    return CacheBucket::kTone;
  }
  // 其余全部保守归 kDisplay：curve./hsl./colorGrade./parametric./optics./
  // masks/basic.{clarity,texture,dehaze,saturation,vibrance}/detail.sharpen*
  return CacheBucket::kDisplay;
}

bool computeCacheKeys(int64_t photoId,
                      const std::vector<std::pair<std::string, std::string>>& params,
                      std::string keys[kCacheBucketCount]) {
  if (keys == nullptr) return false;
  // 按桶分拣（重复 path 后值覆盖前值，map 保序）
  std::vector<std::map<std::string, std::string>> perBucket(kCacheBucketCount);
  for (const auto& kv : params) {
    const int b = static_cast<int>(bucketForParam(kv.first));
    perBucket[b][kv.first] = kv.second;
  }
  const std::string idLine = std::to_string(photoId) + "\n";
  // 层级式键：kGeom 独立；kTone 含自身参数；kDisplay = md5(kTone 键 + 本桶参数)
  // ——上游变更必须让下游缓存失效（换曝光也废 kDisplay，但不废 kGeom）
  for (int b = 0; b < kCacheBucketCount; ++b) {
    std::string blob = (b == static_cast<int>(CacheBucket::kDisplay))
                           ? keys[static_cast<int>(CacheBucket::kTone)]
                           : idLine;
    for (const auto& kv : perBucket[b]) {
      blob += kv.first + "=" + kv.second + "\n";
    }
    keys[b] = md5OfBuffer(blob.data(), blob.size());
    if (keys[b].empty()) return false;  // md5 失败绝不静默
  }
  return true;
}

}  // namespace plan
}  // namespace lumina
