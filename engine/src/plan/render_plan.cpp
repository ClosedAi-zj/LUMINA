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

int stageRank(const std::string& opName) {
  const Operator* op = OpTable::instance().find(opName);
  return op ? stageIndex(op->stage()) : static_cast<int>(Stage::kStageCount);
}

// T1.5：同 stage 内语义序（口径 = CPU oracle oracle93 的逐像素施加顺序——
// 算子不可交换，Plan 序列必须复刻 oracle 链内顺序，否则组合链与预览/oracle 分叉）。
// oracle93 依据：applyWb(temp→tint) → geometry → denoise → localContrast(clarity,texture)
// → applyToneColor[exposure→contrast→zones(hi,sh,wh,bl)→dehaze→primaries
//   (shadowTint,red,green,blue)→vibrance→saturation→toneMap→LUTs(RGB,R,G,B)
//   →parametric→hsl→colorGrade] → sharpen → effects(vignette→grain)。
// 未登记的算子返回大值（stable_sort 保持首现序，向后兼容）。
int semanticRank(const std::string& opName) {
  static const std::map<std::string, int> kRank = {
      // S0
      {"basic.whiteBalance", 0}, {"basic.temperature", 1}, {"basic.tint", 2},
      // S1
      {"lens.profile", 0}, {"lens.manualDistortion", 1},
      {"geometry.crop", 2}, {"geometry.perspective", 3},
      // S1.5
      {"detail.denoise", 0},
      // S2：clarity/texture（oracle localContrast）先于 exposure/contrast/zones；
      //      calibration primaries 先于 vibrance/saturation
      {"basic.clarity", 0},        {"basic.texture", 1},
      {"basic.exposure", 2},       {"basic.contrast", 3},
      {"basic.highlights", 4},     {"basic.shadows", 5},
      {"basic.whites", 6},         {"basic.blacks", 7},
      {"basic.dehaze", 8},
      {"calibration.shadowTint", 9}, {"calibration.red", 10},
      {"calibration.green", 11},   {"calibration.blue", 12},
      {"basic.vibrance", 13},      {"basic.saturation", 14},
      // S3
      {"pipeline.toneMap", 0},
      // S4：点曲线 → 参数曲线 → hsl → colorGrade（oracle applyToneColor 尾段顺序）
      {"iccInput", 0},
      {"curve.toneCurveRGB", 1}, {"curve.toneCurveR", 2},
      {"curve.toneCurveG", 3},   {"curve.toneCurveB", 4},
      {"curve.parametric", 5},   {"color.hsl", 6}, {"color.colorGrade", 7},
      // S5
      {"detail.sharpen", 0},
      // S6
      {"effects.vignette", 0}, {"effects.grain", 1},
      // S7
      {"output.encode", 0},
  };
  const auto it = kRank.find(opName);
  return it != kRank.end() ? it->second : 1000;
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
  // stage 归并稳定排序：同 stage 内按 oracle 语义序（semanticRank），语义序相同的
  // 保持参数出现顺序（iccInput 由语义序保证居前，未知算子保持首现序兜底）
  std::stable_sort(ops.begin(), ops.end(),
                   [](const std::string& a, const std::string& b) {
                     const int sa = stageRank(a);
                     const int sb = stageRank(b);
                     if (sa != sb) return sa < sb;
                     return semanticRank(a) < semanticRank(b);
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
