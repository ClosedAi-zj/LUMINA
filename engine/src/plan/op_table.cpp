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

// ---- 骨架算子注册：8 节点骨架（plan/ops/*.cpp）+ S7 输出占位 ----

namespace {

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
  ok = table->registerOp(std::make_shared<OutputOp>(), err) && ok;
  return ok;
}

}  // namespace plan
}  // namespace lumina
