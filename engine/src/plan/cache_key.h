// T0.5：三键缓存键（架构 §6 缓存层级 / M0 T0.5）
// - kGeom   ：几何派生（裁剪/拉直/透视/镜头畸变）——换曝光不废此键
// - kTone   ：tone map 之前的线性光算子（曝光/白平衡/校准/NR）
// - kDisplay：tone map 之后的感知域算子（曲线/HSL/分级/效果/锐化/蒙版）
// 分桶原则 = 参数变更是否会改变该缓存层像素输出；保守侧（归属存疑）进 kDisplay
#ifndef LUMINA_PLAN_CACHE_KEY_H_
#define LUMINA_PLAN_CACHE_KEY_H_

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace lumina {
namespace plan {

enum class CacheBucket : int {
  kGeom = 0,     // S1 几何输出
  kTone = 1,     // S0–S2 线性光输出（含 NR）
  kDisplay = 2,  // S4–S6 感知域输出
};

constexpr int kCacheBucketCount = 3;

// 参数路径 → 缓存桶。未识别路径保守归 kDisplay（含 masks）。
CacheBucket bucketForParam(const std::string& path);

// 三键计算：photoId + 归一化参数集（path, value 字符串对）。
// 每个 key = md5(photoId "\n" + 本桶相关参数按 path 排序拼接)。
// params 可为空（全默认渲染仍有键）。
bool computeCacheKeys(int64_t photoId,
                      const std::vector<std::pair<std::string, std::string>>& params,
                      std::string keys[kCacheBucketCount]);

}  // namespace plan
}  // namespace lumina

#endif  // LUMINA_PLAN_CACHE_KEY_H_
