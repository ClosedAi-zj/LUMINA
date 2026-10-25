// T1.3 color 节点算子（32 叶：HSL 8 带×3 + 颜色分级 8，CPU oracle，S4 感知码域）
// HSL：RGB（code/65535）↔ HSL，8 区相位权重（hue 距离三角窗，LR ±30° hue / ±100 sat/lum）。
// colorGrade：shadow/mid/high × hue/sat，三分区权重 + balance 移界 + blend 控制交叠。
#include "plan/ops/ops.h"

#include <algorithm>
#include <cmath>

namespace lumina {
namespace plan {
namespace {

// 8 色带中心 hue（0..1）：red orange yellow green aqua blue purple magenta
const double kBandHue[8] = {0.0, 1.0/12.0, 1.0/6.0, 1.0/3.0, 0.5, 2.0/3.0, 5.0/6.0, 11.0/12.0};
const char* kBandName[8] = {"red", "orange", "yellow", "green", "aqua", "blue", "purple", "magenta"};

double hueDist(double a, double b) {
  double d = std::fabs(a - b);
  return d > 0.5 ? 1.0 - d : d;
}

// HSL 单带参数求值：对每像素：hue 权重 w = max(0, 1 - dist/bandWidth)
class HslOp : public ParamOp {
 public:
  HslOp() : ParamOp("color.hsl", Stage::S4Color) {}
  bool apply(imaging::ImageBuf* buf, std::string* err) const override {
    if (!checkBuf(buf, err)) return false;
    // 收集 24 叶参数（缺省 0 = 该叶不动）
    struct Band { double hue, sat, lum; };
    Band bands[8];
    bool any = false;
    for (int i = 0; i < 8; ++i) {
      bands[i].hue = num((std::string("hsl.") + kBandName[i] + ".hue").c_str(), 0.0);
      bands[i].sat = num((std::string("hsl.") + kBandName[i] + ".sat").c_str(), 0.0);
      bands[i].lum = num((std::string("hsl.") + kBandName[i] + ".lum").c_str(), 0.0);
      if (bands[i].hue != 0.0 || bands[i].sat != 0.0 || bands[i].lum != 0.0) any = true;
    }
    if (!any) return true;
    for (size_t px = 0; px + 2 < buf->channelCount(); px += 3) {
      const double r = imaging::codeAt(*buf, px) / 65535.0;
      const double g = imaging::codeAt(*buf, px + 1) / 65535.0;
      const double b = imaging::codeAt(*buf, px + 2) / 65535.0;
      double h, s, l;
      rgbToHsl(r, g, b, &h, &s, &l);
      if (s > 1e-6) {
        double dHue = 0.0, dSat = 0.0, dLum = 0.0;
        for (int i = 0; i < 8; ++i) {
          const double w = std::max(0.0, 1.0 - hueDist(h, kBandHue[i]) / (1.0 / 12.0));
          if (w <= 0.0) continue;
          dHue += bands[i].hue / 100.0 * (30.0 / 360.0) * w;
          dSat += bands[i].sat / 100.0 * w;
          dLum += bands[i].lum / 100.0 * w * 0.3;
        }
        h += dHue;
        if (h < 0.0) h += 1.0;
        if (h >= 1.0) h -= 1.0;
        s = std::clamp(s * (1.0 + dSat), 0.0, 1.0);
        l = std::clamp(l + dLum * 0.3, 0.0, 1.0);
      }
      double rr = r, gg = g, bb = b;
      hslToRgb(h, s, l, &rr, &gg, &bb);
      // TS oracle 同式：写回前 round 到 65535 栅格（float 链下保持该量化语义）
      imaging::setCodeAt(*buf, px, std::round(std::clamp(rr, 0.0, 1.0) * 65535.0));
      imaging::setCodeAt(*buf, px + 1, std::round(std::clamp(gg, 0.0, 1.0) * 65535.0));
      imaging::setCodeAt(*buf, px + 2, std::round(std::clamp(bb, 0.0, 1.0) * 65535.0));
    }
    return true;
  }
};

// 颜色分级（shadow/mid/high × hue/sat + blend + balance）
class ColorGradeOp : public ParamOp {
 public:
  ColorGradeOp() : ParamOp("color.colorGrade", Stage::S4Color) {}
  bool apply(imaging::ImageBuf* buf, std::string* err) const override {
    if (!checkBuf(buf, err)) return false;
    const double sh = num("colorGrade.shadowHue", 0.0);
    const double shs = num("colorGrade.shadowSat", 0.0);
    const double mh = num("colorGrade.midtoneHue", 0.0);
    const double ms = num("colorGrade.midtoneSat", 0.0);
    const double hh = num("colorGrade.highlightHue", 0.0);
    const double hs = num("colorGrade.highlightSat", 0.0);
    const double blend = num("colorGrade.blend", 50.0);
    const double balance = num("colorGrade.balance", 0.0);
    if (sh == 0 && shs == 0 && mh == 0 && ms == 0 && hh == 0 && hs == 0) return true;
    // 三分区边界随 balance 移动（±50 → ±0.15），blend 展宽交叠（0..100 → 0..0.5）
    const double b = balance / 100.0 * 0.15;
    const double spread = 0.25 + blend / 100.0 * 0.25;
    for (size_t px = 0; px + 2 < buf->channelCount(); px += 3) {
      const double r0 = imaging::codeAt(*buf, px) / 65535.0;
      const double g0 = imaging::codeAt(*buf, px + 1) / 65535.0;
      const double b0 = imaging::codeAt(*buf, px + 2) / 65535.0;
      double h, s, l;
      rgbToHsl(r0, g0, b0, &h, &s, &l);
      double r = r0, g = g0, bch = b0;
      if (s > 1e-6 || shs != 0 || ms != 0 || hs != 0) {
        // 分区权重（balance 上移高光边界）
        const double wHi = smooth01(l, 0.5 - b - spread / 2.0, 0.5 - b + spread / 2.0);
        const double wSh = 1.0 - smooth01(l, 0.35 - b - spread / 2.0, 0.35 - b + spread / 2.0);
        const double wMid = std::max(0.0, 1.0 - wHi - wSh);
        struct T { double hue, sat, w; };
        const T t[3] = {{sh, shs, wSh}, {mh, ms, wMid}, {hh, hs, wHi}};
        double dHue = 0.0, dSat = 0.0;
        for (const T& z : t) {
          if (z.w <= 0.0 || z.sat == 0.0) continue;
          // hue 目标：hue 叶 0..360° → 0..1；向目标最短角移动，强度 = |sat|
          const double target = z.hue / 360.0;
          double d = target - h;
          if (d > 0.5) d -= 1.0;
          if (d < -0.5) d += 1.0;
          const double strength = std::min(1.0, std::fabs(z.sat) / 100.0) * z.w;
          dHue += d * strength * 0.5;
          dSat += (z.sat / 100.0) * z.w * 0.3;
        }
        h += dHue;
        if (h < 0.0) h += 1.0;
        if (h >= 1.0) h -= 1.0;
        s = std::clamp(s * (1.0 + dSat), 0.0, 1.0);
        hslToRgb(h, s, l, &r, &g, &bch);
      }
      // TS oracle 同式：写回前 round 到 65535 栅格（float 链下保持该量化语义）
      imaging::setCodeAt(*buf, px, std::round(std::clamp(r, 0.0, 1.0) * 65535.0));
      imaging::setCodeAt(*buf, px + 1, std::round(std::clamp(g, 0.0, 1.0) * 65535.0));
      imaging::setCodeAt(*buf, px + 2, std::round(std::clamp(bch, 0.0, 1.0) * 65535.0));
    }
    return true;
  }
};

}  // namespace

bool registerColorOps(OpTable* table, std::string* err) {
  bool ok = true;
  ok = table->registerOp(std::make_shared<HslOp>(), err) && ok;
  ok = table->registerOp(std::make_shared<ColorGradeOp>(), err) && ok;
  return ok;
}

}  // namespace plan
}  // namespace lumina
