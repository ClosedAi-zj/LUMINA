// T0.5：渲染 Plan（架构 §5 / M0 T0.5）
// - 参数集 → 有序算子序列（stage 归并排序，复用 OpTable 元数据）+ 三键缓存键
// - execute 委托 OpTable::evaluate（stage 校验/域校验/错误透传零静默）
// - 换曝光只改 kTone/kDisplay 键、kGeom 键不变（见 cache_key 分桶）
#ifndef LUMINA_PLAN_RENDER_PLAN_H_
#define LUMINA_PLAN_RENDER_PLAN_H_

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "imaging/image_buffer.h"
#include "plan/cache_key.h"

namespace lumina {
namespace plan {

struct RenderPlan {
  std::vector<std::string> opSequence;               // 有序算子名
  std::string cacheKeys[kCacheBucketCount];          // kGeom/kTone/kDisplay
  bool valid() const { return !cacheKeys[0].empty() && !cacheKeys[1].empty() &&
                              !cacheKeys[2].empty(); }
};

// 编译：photoId + 参数集（path,value；value 已归一化文本）→ RenderPlan。
// 未知参数路径不报错（保守进 kDisplay 键，骨架期不映射算子）。
bool compilePlan(int64_t photoId,
                 const std::vector<std::pair<std::string, std::string>>& params,
                 RenderPlan* plan, std::string* err);

// 求值：按 opSequence 经 OpTable 求值（需 registerBuiltinOps 已执行）。
bool executePlan(const RenderPlan& plan, imaging::ImageBuf* buf, std::string* err);

}  // namespace plan
}  // namespace lumina

#endif  // LUMINA_PLAN_RENDER_PLAN_H_
