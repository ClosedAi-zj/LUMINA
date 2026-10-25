// T1.3 mask 容器节点（1 叶）+ 蒙版求值入口（调用形态现定义，T2.2 实装蒙版数学）
// §3.3 伪码：layer = Op[node](base)；next = mix(base, Blend(layer, base, blendMode),
//                                              a * opacity/100)。
#include "plan/ops/ops.h"

#include <algorithm>
#include <cmath>

namespace lumina {
namespace plan {

// 蒙版求值入口（CPU/GPU 共用同一语义）：
// - op：宿主节点算子（已 setParams）
// - base：主链当前缓冲；mask：0..1 蒙版平面（同 w*h；null = 全 1）
// - opacity 0..100；blendMode：normal|multiply|screen|overlay|softLight
// 返回混合后缓冲写入 out（可与 base 同对象）。
bool applyMasked(const Operator& op, const imaging::ImageBuf& base,
                 const std::vector<float>* mask, int opacity,
                 const std::string& blendMode, imaging::ImageBuf* out,
                 std::string* err) {
  imaging::ImageBuf layer = base;
  if (!op.apply(&layer, err)) return false;
  if (out->w != layer.w || out->h != layer.h || out->data.size() != layer.data.size()) {
    out->w = layer.w;
    out->h = layer.h;
    out->space = layer.space;
    out->logStep = layer.logStep;
    out->data.assign(layer.data.size(), 0);
  }
  const double a = std::max(0.0, std::min(1.0, opacity / 100.0));
  const bool normalOnly = (mask == nullptr);
  for (size_t i = 0; i < layer.data.size(); ++i) {
    double lv = layer.data[i];
    const double bv = base.data[i];
    double blended = lv;
    if (blendMode == "multiply") {
      blended = bv * lv / 65535.0;
    } else if (blendMode == "screen") {
      blended = bv + lv - bv * lv / 65535.0;
    } else if (blendMode == "overlay" || blendMode == "softLight") {
      // 感知码域近似：0.5 为界
      const double t = bv / 65535.0;
      const double s = lv / 65535.0;
      blended = (t < 0.5 ? 2.0 * t * s : 1.0 - 2.0 * (1.0 - t) * (1.0 - s)) * 65535.0;
    }
    double alpha = a;
    if (!normalOnly) {
      alpha *= (*mask)[i / 3];  // 蒙版平面 per-pixel（交错 RGB 共用）
    }
    const double v = bv + (blended - bv) * alpha;
    out->data[i] = static_cast<uint16_t>(std::clamp(v, 0.0, 65535.0) + 0.5);
  }
  return true;
}

namespace {

// mask.apply：宿主侧容器节点——蒙版数学（径向/渐变/画笔/范围）T2.2 实装，
// 骨架期 identity（蒙版几何参数不在此消费）。
class MaskApplyOp : public ParamOp {
 public:
  MaskApplyOp() : ParamOp("mask.apply", Stage::S4Color) {}
  bool apply(imaging::ImageBuf* buf, std::string* err) const override {
    return checkBuf(buf, err);
  }
};

}  // namespace

bool registerMaskOps(OpTable* table, std::string* err) {
  return table->registerOp(std::make_shared<MaskApplyOp>(), err);
}

}  // namespace plan
}  // namespace lumina
