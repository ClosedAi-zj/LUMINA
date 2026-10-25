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
