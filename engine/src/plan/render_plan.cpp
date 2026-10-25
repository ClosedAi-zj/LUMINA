#include "plan/render_plan.h"

#include <algorithm>
#include <map>

#include "plan/op_table.h"

namespace lumina {
namespace plan {

namespace {

// 参数路径 → 算子名（骨架期映射表；M1 逐叶实现后扩充）。
// 返回空串 = 该参数在骨架期不产生独立算子（仅参与缓存键）。
std::string opForParam(const std::string& path) {
  // basic.*：算子名 = 叶路径（basic.cpp 注册口径）
  if (path.rfind("basic.", 0) == 0) return path;
  // calibration
  if (path == "calibration.redHue" || path == "calibration.redSat")
    return "calibration.red";
  if (path == "calibration.greenHue" || path == "calibration.greenSat")
    return "calibration.green";
  if (path == "calibration.blueHue" || path == "calibration.blueSat")
    return "calibration.blue";
  if (path == "calibration.shadowTint") return "calibration.shadowTint";
  // detail：NR 一把算子、锐化一把算子
  if (path == "detail.colorNR" || path == "detail.colorNRDetail" ||
      path == "detail.luminanceSmoothing")
    return "detail.denoise";
  if (path == "detail.sharpness" || path == "detail.sharpenRadius" ||
      path == "detail.sharpenDetail" || path == "detail.sharpenMasking")
    return "detail.sharpen";
  // geometry / lens
  if (path == "geometry.perspective.rotate" ||
      path.rfind("geometry.perspective.", 0) == 0)
    return "geometry.perspective";
  if (path.rfind("geometry.", 0) == 0) return "geometry.crop";
  if (path == "optics.manualDistortion") return "lens.manualDistortion";
  if (path == "optics.lensProfileEnable") return "lens.profile";
  // curve：算子名 = 叶路径
  if (path.rfind("curve.", 0) == 0) return path;
  // 颜色（HSL 全体一把 / 分级一把）
  if (path.rfind("hsl.", 0) == 0) return "color.hsl";
  if (path.rfind("colorGrade.", 0) == 0) return "color.colorGrade";
  // effects
  if (path.rfind("optics.vignette", 0) == 0) return "effects.vignette";
  if (path.rfind("optics.grain", 0) == 0) return "effects.grain";
  // 蒙版
  if (path.rfind("masks", 0) == 0) return "mask.apply";
  // ICC 预留（M2 实装；骨架期映射到 iccInput 占位）
  if (path.rfind("icc.", 0) == 0) return "iccInput";
  return std::string();
}

int stageRank(const std::string& opName) {
  const Operator* op = OpTable::instance().find(opName);
  return op ? stageIndex(op->stage()) : static_cast<int>(Stage::kStageCount);
}

}  // namespace

bool compilePlan(int64_t photoId,
                 const std::vector<std::pair<std::string, std::string>>& params,
                 RenderPlan* plan, std::string* err) {
  if (!plan) {
    if (err) *err = "compilePlan: null plan";
    return false;
  }
  if (!computeCacheKeys(photoId, params, plan->cacheKeys)) {
    if (err) *err = "compilePlan: cache key computation failed";
    return false;
  }
  // 去重收集算子名（同一算子多个叶只进序列一次），记录首次出现顺序
  std::map<std::string, size_t> firstSeen;
  std::vector<std::string> ops;
  for (const auto& kv : params) {
    const std::string opName = opForParam(kv.first);
    if (opName.empty()) continue;
    if (firstSeen.emplace(opName, ops.size()).second) ops.push_back(opName);
  }
  // stage 归并稳定排序：同 stage 保持参数出现顺序（iccInput 由参数顺序保证居前）
  std::stable_sort(ops.begin(), ops.end(),
                   [](const std::string& a, const std::string& b) {
                     return stageRank(a) < stageRank(b);
                   });
  plan->opSequence = std::move(ops);
  return true;
}

bool executePlan(const RenderPlan& plan, imaging::ImageBuf* buf, std::string* err) {
  if (!plan.valid()) {
    if (err) *err = "executePlan: plan not compiled";
    return false;
  }
  return OpTable::instance().evaluate(plan.opSequence, buf, err);
}

}  // namespace plan
}  // namespace lumina
