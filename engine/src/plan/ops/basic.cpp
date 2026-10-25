// basic 节点算子（14 叶：WB 2 + 基础 12）。骨架：exposure 真 implement（log16 加法精确），
// 其余 identity 占位（M1 T1.3 逐叶填实现）。
#include "plan/ops/ops.h"

namespace lumina {
namespace plan {
namespace {

// 曝光（S2，线性光）：log16 域 +EV = code += EV*logStep，2^EV 增益整数精确
class ExposureOp : public Operator {
 public:
  const char* name() const override { return "basic.exposure"; }
  Stage stage() const override { return Stage::S2Tone; }
  bool apply(imaging::ImageBuf* buf, std::string* err) const override {
    if (!buf || !buf->valid()) {
      if (err) *err = "basic.exposure: invalid buffer";
      return false;
    }
    if (ev == 0.0) return true;
    double delta = ev * buf->logStep;
    for (size_t i = 0; i < buf->data.size(); ++i) {
      double c = static_cast<double>(buf->data[i]) + delta;
      if (c < 0.0) c = 0.0;
      if (c > 65535.0) c = 65535.0;
      buf->data[i] = static_cast<uint16_t>(c + 0.5);
    }
    return true;
  }
  double ev = 0.0;  // M1 由 Plan 注入
};

// 白平衡（S0，通道增益）：骨架 identity；M1 实现 temperature/tint → 相机 RGB 增益
class WhiteBalanceOp : public Operator {
 public:
  const char* name() const override { return "basic.whiteBalance"; }
  Stage stage() const override { return Stage::S0Decode; }
  bool apply(imaging::ImageBuf* buf, std::string* err) const override {
    if (!buf || !buf->valid()) {
      if (err) *err = "basic.whiteBalance: invalid buffer";
      return false;
    }
    return true;
  }
};

}  // namespace

bool registerBasicOps(OpTable* table, std::string* err) {
  bool ok = true;
  ok = table->registerOp(std::make_shared<WhiteBalanceOp>(), err) && ok;
  ok = table->registerOp(std::make_shared<ExposureOp>(), err) && ok;
  for (const char* n : {"basic.contrast", "basic.highlights", "basic.shadows", "basic.whites",
                        "basic.blacks", "basic.clarity", "basic.texture", "basic.dehaze",
                        "basic.vibrance", "basic.saturation"}) {
    ok = table->registerOp(std::make_shared<IdentityOp>(n, Stage::S2Tone), err) && ok;
  }
  return ok;
}

}  // namespace plan
}  // namespace lumina
