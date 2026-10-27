// 算子表实现：stage 有序校验 + 骨架算子（M0 T0.3）
#include "plan/op_table.h"

#include "core/logger.h"
#include "plan/ops/ops.h"

namespace lumina {
namespace plan {

namespace {
// 每个 stage 的期望色彩空间域（跨域必须走 plan/color_convert，禁止算子内临时转换）
bool stageExpectsSceneLinear(Stage s) {
  return s == Stage::S0Decode || s == Stage::S1Geometry || s == Stage::S15Denoise ||
         s == Stage::S2Tone;
}
}  // namespace

const char* stageName(Stage s) {
  switch (s) {
    case Stage::S0Decode: return "S0";
    case Stage::S1Geometry: return "S1";
    case Stage::S15Denoise: return "S1.5";
    case Stage::S2Tone: return "S2";
    case Stage::S3ToneMap: return "S3";
    case Stage::S4Color: return "S4";
    case Stage::S5Detail: return "S5";
    case Stage::S6Effects: return "S6";
    case Stage::S7Output: return "S7";
    default: return "S?";
  }
}

OpTable& OpTable::instance() {
  static OpTable table;
  return table;
}

bool OpTable::registerOp(std::shared_ptr<Operator> op, std::string* err) {
  if (!op || !op->name()) {
    if (err) *err = "registerOp: null operator";
    return false;
  }
  if (stageIndex(op->stage()) < 0 || op->stage() >= Stage::kStageCount) {
    if (err) *err = std::string("registerOp: invalid stage for ") + op->name();
    return false;
  }
  auto [it, inserted] = ops_.emplace(op->name(), std::move(op));
  if (!inserted) {
    if (err) *err = std::string("registerOp: duplicate operator ") + it->first;
    return false;
  }
  return true;
}

const Operator* OpTable::find(const std::string& name) const {
  auto it = ops_.find(name);
  return it == ops_.end() ? nullptr : it->second.get();
}

bool OpTable::evaluate(const std::vector<std::string>& opNames, imaging::ImageBuf* buf,
                       std::string* err) const {
  if (!buf || !buf->valid()) {
    if (err) *err = "evaluate: invalid buffer";
    return false;
  }
  int current = 0;  // 已到达的 stage 序号
  for (const std::string& name : opNames) {
    const Operator* op = find(name);
    if (!op) {
      if (err) *err = "evaluate: unknown operator '" + name + "'";
      return false;
    }
    // stage 错序：运行期报错（骨架期硬失败；M1 Plan 求值器会按 stage 分桶调度）
    if (!stageAllowedAfter(op->stage(), static_cast<Stage>(current))) {
      if (err) *err = "evaluate: stage order violation: '" + name + "' (" +
                      stageName(op->stage()) + ") after stage " + stageName(static_cast<Stage>(current - 1));
      return false;
    }
    // 域校验：scene-linear 算子不得吃 display 域缓冲（S3 之后）
    if (stageExpectsSceneLinear(op->stage()) &&
        stageIndex(op->stage()) <= stageIndex(Stage::S2Tone) &&
        current > stageIndex(Stage::S3ToneMap)) {
      if (err) *err = "evaluate: domain violation: scene-linear op '" + name +
                      "' scheduled after ToneMap";
      return false;
    }
    if (!op->apply(buf, err)) {
      if (err && err->empty()) *err = "evaluate: operator '" + name + "' failed";
      return false;
    }
    // 推进到本算子的 stage（不含 +1：同 stage 的后续算子允许，仅禁止回到更早 stage）
    if (stageIndex(op->stage()) > current) current = stageIndex(op->stage());
  }
  return true;
}

bool OpTable::evaluate(const std::vector<std::string>& opNames, imaging::ImageBuf* buf,
                       const std::map<std::string, json::JsonValue>& paramsByOp,
                       std::string* err) {
  // 先统一注入参数（本序列不重复的算子不受影响）
  for (const std::string& name : opNames) {
    auto it = paramsByOp.find(name);
    Operator* op = const_cast<Operator*>(find(name));
    if (!op) {
      if (err) *err = "evaluate: unknown operator '" + name + "'";
      return false;
    }
    op->setParams(it != paramsByOp.end() ? it->second : json::JsonValue::objectValue());
  }
  return evaluate(opNames, buf, err);
}

// ---- 骨架算子注册：8 节点骨架（plan/ops/*.cpp）+ iccInput 预留 + S7 输出占位 ----

namespace {

// iccInput 预留位（lead 裁决：ICC 输入 profile 提前到 M2）。
// stage = S4Color 前段：调用方（compilePlan/求值序列）必须把它排在 S4 首位。
// 骨架期 identity；M2 实装：按 buf->iccProfileId 做输入 profile → 工作域转换。
class IccInputOp : public Operator {
 public:
  const char* name() const override { return "iccInput"; }
  Stage stage() const override { return Stage::S4Color; }
  bool apply(imaging::ImageBuf* buf, std::string* err) const override {
    if (!buf || !buf->valid()) {
      if (err) *err = "iccInput: invalid buffer";
      return false;
    }
    return true;  // M2 前占位 identity
  }
};

// S3 ToneMap（唯一域分界，非节点）：scene-linear → display-referred。
// CPU oracle 口径：Reinhard c/(1+c)（单调、0→0、∞→1）；中间值 0.18 → 0.153。
// T2.1 管线重排时由 PlanBuilder 在 S2/S4 之间自动插入。
class ToneMapOp : public Operator {
 public:
  const char* name() const override { return "pipeline.toneMap"; }
  Stage stage() const override { return Stage::S3ToneMap; }
  bool apply(imaging::ImageBuf* buf, std::string* err) const override {
    if (!buf || !buf->valid()) {
      if (err) *err = "pipeline.toneMap: invalid buffer";
      return false;
    }
    for (size_t i = 0; i < buf->channelCount(); ++i) {
      const double c = imaging::log16DecodeF(imaging::codeAt(*buf, i), buf->logStep);
      const double d = c / (1.0 + c);
      // log16Encode 自带 round-to-栅格（TS oracle 同式：编码点量化语义保留）
      imaging::setCodeAt(*buf, i,
                         static_cast<double>(imaging::log16Encode(static_cast<float>(d), buf->logStep)));
    }
    return true;
  }
};

// 输出编码占位（S7）：骨架 identity；M0 T0.7 实现 8bit TPDF/16bit 线性写出
class OutputOp : public Operator {
 public:
  const char* name() const override { return "output.encode"; }
  Stage stage() const override { return Stage::S7Output; }
  bool apply(imaging::ImageBuf* buf, std::string* err) const override {
    if (!buf || !buf->valid()) {
      if (err) *err = "output.encode: invalid buffer";
      return false;
    }
    return true;
  }
};

}  // namespace

// ---------------- 20 阶段顺序表（M2 T2.1，架构 §3.7） ----------------
// 纪律：CI 只读；重排/删行 = MR 拒绝（S10）。第 2–6 行是 geometry.apply 的内部子步
// （subStep 1..6），算子级子序列断言只看 subStep == 0 的行。
const PipelineStageDesc kPipelineOrder[kPipelineOrderCount] = {
    {1, "白平衡通道增益", Stage::S0Decode,
     "basic.whiteBalance,basic.temperature,basic.tint", 0, Stage::S0Decode,
     "scene-linear camera"},
    {2, "镜头 profile 畸变（k1–k3）", Stage::S1Geometry, "geometry.apply", 1, Stage::S1Geometry,
     "scene-linear working"},
    {3, "色差校正（CA）", Stage::S1Geometry, "", 2, Stage::S1Geometry, "scene-linear working"},
    {4, "光学暗角校正", Stage::S1Geometry, "", 3, Stage::S1Geometry, "scene-linear working"},
    {5, "透视/变换（含 aspect）", Stage::S1Geometry, "", 4, Stage::S1Geometry,
     "scene-linear working"},
    {6, "裁剪 + 拉直 + 唯一一次重采样", Stage::S1Geometry, "", 6, Stage::S1Geometry,
     "scene-linear working"},
    {7, "降噪（色度→亮度）", Stage::S15Denoise, "detail.denoise", 0, Stage::S15Denoise,
     "scene-linear working"},
    {8, "局部对比", Stage::S2Tone, "basic.clarity,basic.texture", 0, Stage::S2Tone,
     "scene-linear working"},
    {9, "曝光", Stage::S2Tone, "basic.exposure", 0, Stage::S2Tone, "scene-linear working"},
    {10, "对比", Stage::S2Tone, "basic.contrast", 0, Stage::S2Tone, "scene-linear working"},
    {11, "高光/阴影/白/黑场", Stage::S2Tone,
     "basic.highlights,basic.shadows,basic.whites,basic.blacks", 0, Stage::S2Tone,
     "scene-linear working"},
    {12, "去雾", Stage::S2Tone, "basic.dehaze", 0, Stage::S2Tone, "scene-linear working"},
    {13, "原色校准", Stage::S2Tone,
     "calibration.shadowTint,calibration.red,calibration.green,calibration.blue", 0,
     Stage::S2Tone, "scene-linear working"},
    {14, "自然饱和度/饱和度", Stage::S2Tone, "basic.vibrance,basic.saturation", 0, Stage::S2Tone,
     "scene-linear working"},
    {15, "tone map（唯一域分界）", Stage::S3ToneMap, "pipeline.toneMap", 0, Stage::S3ToneMap,
     "display-referred"},
    {16, "曲线（复合/分通道/参数）", Stage::S4Color,
     "curve.toneCurveRGB,curve.toneCurveR,curve.toneCurveG,curve.toneCurveB,curve.parametric", 0,
     Stage::S4Color, "display"},
    {17, "HSL / 颜色分级", Stage::S4Color, "color.hsl,color.colorGrade", 0, Stage::S4Color,
     "display"},
    {18, "蒙版局部调整（M2 新增）", Stage::S4Color, "mask.apply", 0, Stage::S4Color, "display"},
    {19, "锐化", Stage::S5Detail, "detail.sharpen", 0, Stage::S5Detail, "display"},
    {20, "裁后暗角/颗粒 + 输出编码", Stage::S6Effects,
     "effects.vignette,effects.grain,output.encode", 0, Stage::S7Output, "display → 8/16bit"},
};

// 算子名 → 所在行号（1..20）；未登记返回 0。由 pipelineRankForOp 派生（单一事实源）。
int pipelineRowForOp(const std::string& opName) {
  int rank = 0;
  return pipelineRankForOp(opName, &rank, nullptr) ? rank / 100 : 0;
}

// T2.10 表驱动（§3.7.1）：rank = 行号 * 100 + 行内算子下标，全部由 kPipelineOrder
// 派生。未登记 → err + false（硬失败，无兜底）；旧 S1 算子名的兼容映射已删除。
bool pipelineRankForOp(const std::string& opName, int* rankOut, std::string* err) {
  if (opName.empty()) {
    if (err) *err = "pipelineRankForOp: empty op name";
    return false;
  }
  for (int i = 0; i < kPipelineOrderCount; ++i) {
    const std::string ops = kPipelineOrder[i].ops;
    size_t start = 0;
    int idx = 0;
    while (start <= ops.size()) {
      size_t comma = ops.find(',', start);
      if (comma == std::string::npos) comma = ops.size();
      if (ops.compare(start, comma - start, opName) == 0) {
        if (rankOut != nullptr) *rankOut = kPipelineOrder[i].index * 100 + idx;
        return true;
      }
      if (comma == ops.size()) break;
      start = comma + 1;
      ++idx;
    }
  }
  // 例外白名单：iccInput（M4 实装时正式登记进表；S4 首位，暂归第 15/16 行界）
  if (opName == "iccInput") {
    if (rankOut != nullptr) *rankOut = 15 * 100 + 50;
    return true;
  }
  if (err) *err = "pipelineRankForOp: op '" + opName + "' not in kPipelineOrder（T2.10 表驱动硬失败）";
  return false;
}

bool registerBuiltinOps(OpTable* table, std::string* err) {
  bool ok = true;
  ok = registerBasicOps(table, err) && ok;
  ok = registerCurveOps(table, err) && ok;
  ok = registerColorOps(table, err) && ok;
  ok = registerCalibrationOps(table, err) && ok;
  ok = registerDetailOps(table, err) && ok;
  ok = registerLensOps(table, err) && ok;
  ok = registerGeometryOps(table, err) && ok;
  ok = registerEffectsOps(table, err) && ok;
  ok = registerMaskOps(table, err) && ok;
  ok = table->registerOp(std::make_shared<ToneMapOp>(), err) && ok;
  ok = table->registerOp(std::make_shared<IccInputOp>(), err) && ok;
  ok = table->registerOp(std::make_shared<OutputOp>(), err) && ok;
  return ok;
}

}  // namespace plan
}  // namespace lumina
