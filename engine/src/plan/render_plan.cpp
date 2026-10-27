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
  // geometry / lens：T2.10 收口（架构 §4.3）——S1 六个旧算子名退役，全部路由到
  // 单一算子 geometry.apply（一次重采样，O3/O4；子序由 test_geometry_chain 断言）
  if (path.rfind("geometry.", 0) == 0) return "geometry.apply";
  if (path == "optics.manualDistortion") return "geometry.apply";
  if (path == "optics.lensProfileEnable") return "geometry.apply";
  // T2.11：CA 新叶 #94 + profile 匹配输入 → geometry.apply（S1 子步 1/2/3 消费）
  if (path == "optics.chromaticAberration") return "geometry.apply";
  if (path == "optics.lensProfileMake" || path == "optics.lensProfileModel" ||
      path == "optics.lensProfileLensMake" || path == "optics.lensProfileLensModel" ||
      path == "optics.lensProfileFocalMm")
    return "geometry.apply";
  // curve：点曲线算子名 = 叶路径；参数曲线 7 叶收拢到 curve.parametric
  if (path.rfind("curve.", 0) == 0) return path;
  if (path.rfind("parametric.", 0) == 0) return "curve.parametric";
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

// T1.5：缓存键取值的稳定文本形式（字符串原样，其余 serialize）。
std::string valueKeyString(const json::JsonValue& v) {
  if (v.isString()) return v.asString();
  return v.serialize();
}

}  // namespace

bool compilePlan(int64_t photoId,
                 const std::vector<std::pair<std::string, std::string>>& params,
                 RenderPlan* plan, std::string* err) {
  // 字符串契约委托 JsonValue 重载（值原样包成字符串 JsonValue）
  std::vector<std::pair<std::string, json::JsonValue>> kvs;
  kvs.reserve(params.size());
  for (const auto& kv : params) kvs.push_back({kv.first, json::JsonValue::str(kv.second)});
  return compilePlan(photoId, kvs, plan, err);
}

bool compilePlan(int64_t photoId,
                 const std::vector<std::pair<std::string, json::JsonValue>>& params,
                 RenderPlan* plan, std::string* err) {
  if (!plan) {
    if (err) *err = "compilePlan: null plan";
    return false;
  }
  // 缓存键仍按文本契约计算（JsonValue → 稳定文本形式）
  std::vector<std::pair<std::string, std::string>> kvTexts;
  kvTexts.reserve(params.size());
  for (const auto& kv : params) kvTexts.push_back({kv.first, valueKeyString(kv.second)});
  if (!computeCacheKeys(photoId, kvTexts, plan->cacheKeys)) {
    if (err) *err = "compilePlan: cache key computation failed";
    return false;
  }
  // 去重收集算子名（同一算子多个叶只进序列一次），记录首次出现顺序；
  // 同时收集算子参数（叶路径键值原样透传给算子，键口径 = compile 入参 kv.first）
  std::map<std::string, size_t> firstSeen;
  std::vector<std::string> ops;
  for (const auto& kv : params) {
    const std::string opName = opForParam(kv.first);
    if (opName.empty()) continue;
    if (firstSeen.emplace(opName, ops.size()).second) ops.push_back(opName);
    plan->paramsByOp[opName].setItem(kv.first, kv.second);
  }
  // T1.5：pipeline.toneMap 恒入序列（架构 §5.2：S3 是唯一域分界、非节点算子；
  // 预览 GPU 管线与 TS oracle 均无条件施加 Reinhard——导出走 Plan 必须同域，
  // 否则导出像素整体处在 scene-linear 域、与预览不可比）。stage 归并自动落 S3 位。
  if (firstSeen.emplace("pipeline.toneMap", ops.size()).second) {
    ops.push_back("pipeline.toneMap");
  }
  // T2.10 表驱动排序（§3.7.1 第 1/2 条）：rank 由 kPipelineOrder 派生（pipelineRankForOp，
  // 单一事实源）。未登记算子 → 硬失败（err + false），**不再有 semanticRank 的
  // return 1000 兜底**（S3 零静默：表与实现漂移在编译期暴露，而不是静默排尾）。
  // 行号单调蕴含 stage 单调（表本身 stage 单调不减），单一 rank 全序即可。
  std::map<std::string, int> ranks;
  for (const std::string& name : ops) {
    int rank = 0;
    if (!pipelineRankForOp(name, &rank, err)) {
      *err = "compilePlan: " + *err;
      return false;
    }
    ranks.emplace(name, rank);
  }
  std::stable_sort(ops.begin(), ops.end(),
                   [&ranks](const std::string& a, const std::string& b) {
                     return ranks[a] < ranks[b];
                   });
  plan->opSequence = std::move(ops);
  return true;
}

bool executePlan(const RenderPlan& plan, imaging::ImageBuf* buf, std::string* err) {
  if (!plan.valid()) {
    if (err) *err = "executePlan: plan not compiled";
    return false;
  }
  // T1.5：Plan 求值全程 float32 工作链（GPU 预览同精度语义）。
  // uint16 仍是存储/缓存权威（架构 D1）：开链时 data→dataF，链尾一次性 quantize 回 data。
  // 逐算子直调（ctest/宿主复用）不经过本函数，仍走 uint16 历史语义。
  if (!buf || !buf->valid()) {
    if (err) *err = "executePlan: invalid buffer";
    return false;
  }
  const size_t n = buf->channelCount();
  buf->dataF.resize(n);
  for (size_t i = 0; i < n; ++i) buf->dataF[i] = static_cast<float>(buf->data[i]);
  const bool ok = OpTable::instance().evaluate(plan.opSequence, buf, plan.paramsByOp, err);
  if (ok && buf->floatMode()) {
    // 几何算子可能中途改尺寸：以链尾实际尺寸为准 quantize 回 uint16 权威缓冲
    const size_t n2 = buf->channelCount();
    buf->data.resize(n2);
    for (size_t i = 0; i < n2; ++i) {
      float v = buf->dataF[i];
      if (v < 0.0f) v = 0.0f;
      if (v > 65535.0f) v = 65535.0f;
      buf->data[i] = static_cast<uint16_t>(v + 0.5f);
    }
  }
  buf->dataF.clear();
  return ok;
}

}  // namespace plan
}  // namespace lumina
