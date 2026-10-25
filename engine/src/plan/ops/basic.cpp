// T1.3 basic 节点算子（14 叶，CPU oracle）
// 域：S0 WB 通道增益 / S2 基础色调 —— 全部线性光 log16 码域（码值偏移 = 线性增益）。
// 每叶语义：identity@0、单调、值域 clamp。局部对比类（clarity/texture）用盒式模糊近似。
#include "plan/ops/ops.h"

#include <algorithm>
#include <vector>

namespace lumina {
namespace plan {
namespace {

constexpr double kMidCode = 65535.0 * 0.45;  // 感知中间调锚点（略低于中点，贴近 LR 观感）

// ---- S2 分区权重锚点（T1.5b：log16 t 域显示参考标定）----
// t = code/65535（log16 场景线性码，t = (12+log2(L))/32）。显示参考点换算：
//   显示白 sRGB255 → t=0.3750；75% → 0.3456；50% → 0.3059；25% → 0.2411；10% → 0.1689。
// 旧实现把「显示参考域」阈值（0.5/1.0/0.75/0.25）直接用于 t 域：
//   highlights/whites 对一切显示参考像素恒 0（死算子），shadows 打全图。
// LR 语义定稿（四条锚点）：
//   highlights：只吃接近白的高光——显示 ≈47%（t=0.300）起坡，显示白满权重，
//               超白（RAW 裁切，t>0.375）恒满权重；
//   whites：只吃最亮端——显示 75%（t=0.3456）起坡，显示白满权重；
//   shadows：只吃暗部——显示 25% 以下满权重，显示 50% 以上归零；
//   blacks：只吃最黑端——码 0 满权重，显示 10%（t=0.1689）以上归零。
inline double wHigh(double t)   { return smooth01(t, 0.3000, 0.3750); }
inline double wWhites(double t) { return smooth01(t, 0.3456, 0.3750); }
inline double wShadow(double t) { return 1.0 - smooth01(t, 0.2410, 0.3060); }
inline double wBlacks(double t) { return 1.0 - smooth01(t, 0.0, 0.1689); }

// 曝光（S2）：log16 +EV = code += EV*logStep（整数精确，T0.3 语义不变）
class ExposureOp : public ParamOp {
 public:
  ExposureOp() : ParamOp("basic.exposure", Stage::S2Tone) {}
  bool apply(imaging::ImageBuf* buf, std::string* err) const override {
    if (!checkBuf(buf, err)) return false;
    const double ev = num("basic.exposure", 0.0);
    if (ev == 0.0) return true;
    const double delta = ev * buf->logStep;
    for (size_t i = 0; i < buf->channelCount(); ++i) {
      const double c = static_cast<double>(imaging::codeAt(*buf, i)) + delta;
      imaging::setCodeAt(*buf, i, c);
    }
    return true;
  }
};

// 白平衡选择器（asShot/custom）：无独立像素效果（增益由 temperature/tint 承载）
class WhiteBalanceOp : public ParamOp {
 public:
  WhiteBalanceOp() : ParamOp("basic.whiteBalance", Stage::S0Decode) {}
  bool apply(imaging::ImageBuf* buf, std::string* err) const override {
    return checkBuf(buf, err);
  }
};

// WB 增益基座：码域 EV 偏移（S0）
class WbGainOp : public ParamOp {
 public:
  WbGainOp(const char* n) : ParamOp(n, Stage::S0Decode) {}
 protected:
  // ev[r/g/b] = 各通道 EV 偏移；码值 += ev*logStep
  void shiftChannels(imaging::ImageBuf* buf, double evR, double evG, double evB) const {
    const double d[3] = {evR * buf->logStep, evG * buf->logStep, evB * buf->logStep};
    for (size_t i = 0; i + 2 < buf->channelCount(); i += 3) {
      for (int c = 0; c < 3; ++c) {
        imaging::setCodeAt(*buf, i + c, static_cast<double>(imaging::codeAt(*buf, i + c)) + d[c]);
      }
    }
  }
};

// 色温（S0）：temp>0 → 暖（R 升 B 降）；100 单位 ≈ ±0.2EV
class TemperatureOp : public WbGainOp {
 public:
  TemperatureOp() : WbGainOp("basic.temperature") {}
  bool apply(imaging::ImageBuf* buf, std::string* err) const override {
    if (!checkBuf(buf, err)) return false;
    const double t = num("basic.temperature", 0.0);
    if (t == 0.0) return true;
    const double ev = t * 0.002;
    shiftChannels(buf, ev, 0.0, -ev);
    return true;
  }
};

// 色调（S0）：tint>0 → 品红（R/B 升 G 降）
class TintOp : public WbGainOp {
 public:
  TintOp() : WbGainOp("basic.tint") {}
  bool apply(imaging::ImageBuf* buf, std::string* err) const override {
    if (!checkBuf(buf, err)) return false;
    const double t = num("basic.tint", 0.0);
    if (t == 0.0) return true;
    const double ev = t * 0.001;
    shiftChannels(buf, ev * 0.5, -ev, ev * 0.5);
    return true;
  }
};

// 对比度（S2）：围绕感知中间调的线性拉伸（S 曲线低阶近似），per-channel
class ContrastOp : public ParamOp {
 public:
  ContrastOp() : ParamOp("basic.contrast", Stage::S2Tone) {}
  bool apply(imaging::ImageBuf* buf, std::string* err) const override {
    if (!checkBuf(buf, err)) return false;
    const double k = 1.0 + num("basic.contrast", 0.0) / 100.0 * 0.3;
    if (k == 1.0) return true;
    for (size_t i = 0; i < buf->channelCount(); ++i) {
      const double v = kMidCode + (static_cast<double>(imaging::codeAt(*buf, i)) - kMidCode) * k;
      imaging::setCodeAt(*buf, i, v);
    }
    return true;
  }
};

// 高光/阴影/白色色阶/黑色色阶（S2）：按亮度分区的码值偏移
class ToneZoneOp : public ParamOp {
 public:
  enum Zone { kHighlights, kShadows, kWhites, kBlacks };
  ToneZoneOp(const char* n, Zone z) : ParamOp(n, Stage::S2Tone), z_(z) {}
  bool apply(imaging::ImageBuf* buf, std::string* err) const override {
    if (!checkBuf(buf, err)) return false;
    const double amt = num(n_, 0.0);
    if (amt == 0.0) return true;
    const double k = amt / 100.0 * 3000.0;  // 满量程 ≈ 1.5EV
    for (size_t i = 0; i + 2 < buf->channelCount(); i += 3) {
      const double l = perceptualLumAt(*buf, i);
      double w;
      switch (z_) {
        case kHighlights: w = wHigh(l); break;
        case kShadows: w = wShadow(l); break;
        case kWhites: w = wWhites(l); break;
        default: w = wBlacks(l); break;
      }
      const double d = k * w;
      for (int c = 0; c < 3; ++c) {
        imaging::setCodeAt(*buf, i + c, static_cast<double>(imaging::codeAt(*buf, i + c)) + d);
      }
    }
    return true;
  }
 private:
  Zone z_;
};

// 局部对比基座：out = base + k*(base - blur(base, radius))，码域 per-channel
class LocalContrastOp : public ParamOp {
 public:
  LocalContrastOp(const char* n, int radius, double gainScale, Stage s)
      : ParamOp(n, s), radius_(radius), gain_(gainScale) {}
  bool apply(imaging::ImageBuf* buf, std::string* err) const override {
    if (!checkBuf(buf, err)) return false;
    const double amt = num(n_, 0.0);
    if (amt == 0.0) return true;
    const double k = amt / 100.0 * gain_;
    const int w = buf->w;
    const int h = buf->h;
    std::vector<float> plane(static_cast<size_t>(w) * h);
    std::vector<float> blurred(static_cast<size_t>(w) * h);
    for (int c = 0; c < 3; ++c) {
      for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
          plane[static_cast<size_t>(y) * w + x] =
              imaging::codeAt(*buf, (static_cast<size_t>(y) * w + x) * 3 + c);
        }
      }
      boxBlurPlane(plane.data(), blurred.data(), w, h, radius_);
      for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
          const size_t idx = (static_cast<size_t>(y) * w + x) * 3 + c;
          const double base = imaging::codeAt(*buf, idx);
          const double resid = base - blurred[static_cast<size_t>(y) * w + x];
          imaging::setCodeAt(*buf, idx, base + k * resid);
        }
      }
    }
    return true;
  }
 private:
  int radius_;
  double gain_;
};

// 去雾（S2）：oracle = 0.5×阴影提升 + 0.25×黑色下降 + 0.3×自然饱和（简化组合，T2.3 精修）
class DehazeOp : public ParamOp {
 public:
  DehazeOp() : ParamOp("basic.dehaze", Stage::S2Tone) {}
  bool apply(imaging::ImageBuf* buf, std::string* err) const override {
    if (!checkBuf(buf, err)) return false;
    const double dz = num("basic.dehaze", 0.0);
    if (dz == 0.0) return true;
    const double kShadow = dz / 100.0 * 1500.0;
    const double kBlack = -dz / 100.0 * 500.0;   // 压黑增加通透感
    const double kSat = dz / 100.0 * 0.25;
    for (size_t i = 0; i + 2 < buf->channelCount(); i += 3) {
      const double l = perceptualLumAt(*buf, i);
      const double mean = (imaging::codeAt(*buf, i) + imaging::codeAt(*buf, i + 1) +
                           imaging::codeAt(*buf, i + 2)) /
                          3.0;
      for (int c = 0; c < 3; ++c) {
        double v = imaging::codeAt(*buf, i + c);
        v += kShadow * wShadow(l) + kBlack * wBlacks(l);  // T1.5b：同源 t 域锚点
        v = mean + (v - mean) * (1.0 + kSat);  // 轻度饱和
        imaging::setCodeAt(*buf, i + c, v);
      }
    }
    return true;
  }
};

// 饱和基座：out = mean + (c - mean)*factor（码域）
class SaturationOp : public ParamOp {
 public:
  SaturationOp(const char* n, bool vibrance) : ParamOp(n, Stage::S2Tone), vib_(vibrance) {}
  bool apply(imaging::ImageBuf* buf, std::string* err) const override {
    if (!checkBuf(buf, err)) return false;
    const double amt = num(n_, 0.0);
    if (amt == 0.0) return true;
    for (size_t i = 0; i + 2 < buf->channelCount(); i += 3) {
      const double mean = (imaging::codeAt(*buf, i) + imaging::codeAt(*buf, i + 1) +
                           imaging::codeAt(*buf, i + 2)) /
                          3.0;
      double factor = 1.0 + amt / 100.0;
      if (vib_) {
        // 自然饱和：饱和度越低增益越大（保护高饱和像素）
        const double r = imaging::codeAt(*buf, i);
        const double g = imaging::codeAt(*buf, i + 1);
        const double b = imaging::codeAt(*buf, i + 2);
        const double mx = std::max(r, std::max(g, b));
        const double mn = std::min(r, std::min(g, b));
        const double sat = (mx - mn) / (mx + mn + 1.0);
        factor = 1.0 + amt / 100.0 * (1.0 - sat) * 0.6;
      }
      for (int c = 0; c < 3; ++c) {
        const double v = mean + (imaging::codeAt(*buf, i + c) - mean) * factor;
        imaging::setCodeAt(*buf, i + c, v);
      }
    }
    return true;
  }
 private:
  bool vib_;
};

}  // namespace

bool registerBasicOps(OpTable* table, std::string* err) {
  bool ok = true;
  ok = table->registerOp(std::make_shared<WhiteBalanceOp>(), err) && ok;
  ok = table->registerOp(std::make_shared<TemperatureOp>(), err) && ok;
  ok = table->registerOp(std::make_shared<TintOp>(), err) && ok;
  ok = table->registerOp(std::make_shared<ExposureOp>(), err) && ok;
  ok = table->registerOp(std::make_shared<ContrastOp>(), err) && ok;
  ok = table->registerOp(std::make_shared<ToneZoneOp>("basic.highlights", ToneZoneOp::kHighlights), err) && ok;
  ok = table->registerOp(std::make_shared<ToneZoneOp>("basic.shadows", ToneZoneOp::kShadows), err) && ok;
  ok = table->registerOp(std::make_shared<ToneZoneOp>("basic.whites", ToneZoneOp::kWhites), err) && ok;
  ok = table->registerOp(std::make_shared<ToneZoneOp>("basic.blacks", ToneZoneOp::kBlacks), err) && ok;
  ok = table->registerOp(std::make_shared<LocalContrastOp>("basic.clarity", 10, 0.6, Stage::S2Tone), err) && ok;
  ok = table->registerOp(std::make_shared<LocalContrastOp>("basic.texture", 2, 0.8, Stage::S2Tone), err) && ok;
  ok = table->registerOp(std::make_shared<DehazeOp>(), err) && ok;
  ok = table->registerOp(std::make_shared<SaturationOp>("basic.vibrance", true), err) && ok;
  ok = table->registerOp(std::make_shared<SaturationOp>("basic.saturation", false), err) && ok;
  return ok;
}

}  // namespace plan
}  // namespace lumina
