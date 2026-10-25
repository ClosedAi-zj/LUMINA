// T1.3 calibration 节点算子（7 叶：阴影/红/绿/蓝原色，CPU oracle，S2 线性光码域）
// 原色 hue：通道交叉混合（主色向邻色旋转）；sat：主色饱和（离亮度差缩放）；
// shadowTint：仅阴影区的绿-品红偏移。全部 identity@0、clamp。
#include "plan/ops/ops.h"

#include <algorithm>

namespace lumina {
namespace plan {
namespace {

// 原色基座：channel = 0/1/2（R/G/B），读 "<prefix>.<name>Hue/Sat"
class PrimaryOp : public ParamOp {
 public:
  PrimaryOp(const char* n, const char* hueKey, const char* satKey, int ch)
      : ParamOp(n, Stage::S2Tone), hueKey_(hueKey), satKey_(satKey), ch_(ch) {}
  bool apply(imaging::ImageBuf* buf, std::string* err) const override {
    if (!checkBuf(buf, err)) return false;
    const double hue = num(hueKey_, 0.0);
    const double sat = num(satKey_, 0.0);
    if (hue == 0.0 && sat == 0.0) return true;
    const int o1 = (ch_ + 1) % 3;
    const int o2 = (ch_ + 2) % 3;
    for (size_t i = 0; i + 2 < buf->channelCount(); i += 3) {
      double px[3] = {static_cast<double>(imaging::codeAt(*buf, i)),
                      static_cast<double>(imaging::codeAt(*buf, i + 1)),
                      static_cast<double>(imaging::codeAt(*buf, i + 2))};
      const double mean = (px[0] + px[1] + px[2]) / 3.0;
      // hue：主色与两邻色交叉混合（hue>0 → 向 hue 轴正方向旋转）
      const double cross = (px[o1] - px[o2]) * (hue / 100.0 * 0.25);
      px[ch_] += cross;
      // sat：主色离亮度差缩放
      px[ch_] = mean + (px[ch_] - mean) * (1.0 + sat / 100.0);
      for (int c = 0; c < 3; ++c) {
        imaging::setCodeAt(*buf, i + c, px[c]);
      }
    }
    return true;
  }
 private:
  const char* hueKey_;
  const char* satKey_;
  int ch_;
};

// 阴影色调（绿-品红）：仅阴影区，tint>0 → 品红（R/B 升 G 降）
class ShadowTintOp : public ParamOp {
 public:
  ShadowTintOp() : ParamOp("calibration.shadowTint", Stage::S2Tone) {}
  bool apply(imaging::ImageBuf* buf, std::string* err) const override {
    if (!checkBuf(buf, err)) return false;
    const double t = num("calibration.shadowTint", 0.0);
    if (t == 0.0) return true;
    const double dR = t / 100.0 * 400.0 * 0.5;
    const double dG = -t / 100.0 * 400.0;
    const double dB = dR;
    for (size_t i = 0; i + 2 < buf->channelCount(); i += 3) {
      const double l = perceptualLumAt(*buf, i);
      const double w = 1.0 - smooth01(l, 0.0, 0.4);  // 只影响阴影
      imaging::setCodeAt(*buf, i, imaging::codeAt(*buf, i) + dR * w);
      imaging::setCodeAt(*buf, i + 1, imaging::codeAt(*buf, i + 1) + dG * w);
      imaging::setCodeAt(*buf, i + 2, imaging::codeAt(*buf, i + 2) + dB * w);
    }
    return true;
  }
};

}  // namespace

bool registerCalibrationOps(OpTable* table, std::string* err) {
  bool ok = true;
  ok = table->registerOp(std::make_shared<ShadowTintOp>(), err) && ok;
  ok = table->registerOp(std::make_shared<PrimaryOp>(
      "calibration.red", "calibration.redHue", "calibration.redSat", 0), err) && ok;
  ok = table->registerOp(std::make_shared<PrimaryOp>(
      "calibration.green", "calibration.greenHue", "calibration.greenSat", 1), err) && ok;
  ok = table->registerOp(std::make_shared<PrimaryOp>(
      "calibration.blue", "calibration.blueHue", "calibration.blueSat", 2), err) && ok;
  return ok;
}

}  // namespace plan
}  // namespace lumina
