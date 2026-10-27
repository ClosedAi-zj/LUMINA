// M2 T2.6 验收（N21）：蒙版数学引擎（CPU）
//
// A1：单蒙版 5 类（linear/radial/brush/luminanceRange/colorRange）× 3 组局部参数
//     vs 独立双精度参考实现：mean|Δ| ≤ 2/255、max|Δ| ≤ 6/255（8bit LSB 口径）。
//     参考实现按架构 §3.1.4 公式表独立重写（不复用 mask.cpp 的施加代码），
//     仅共享 §3.1.4 声明依赖的原语（smooth01/rgbToHsl/boxBlurPlane 语义）与
//     画笔场生成（buildBrushField256 是 S7 要与 TS 交叉断言的镜像权威）。
// A2：区外 ≤1 LSB、区内 ±1EV ≥8/255（蒙版分区口径）。
// A4：20 槽逐个关闭均产生差异 + 全 replace 硬边蒙版置换数组序结果逐字节不变
//     （fold 只依赖数组序+combine，§3.1.3）。
// A7：分区口径方向正确（linear 梯度方向 / temperature R>B / highlights 亮区强）。
// 羽化边界抽样：radial feather 场单调 + 端点值；brush 场中心值/边界值。
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

#include "imaging/image_buffer.h"
#include "json/json_value.h"
#include "plan/op_table.h"
#include "plan/ops/mask_field.h"
#include "plan/ops/ops.h"
#include "plan/render_plan.h"

using namespace lumina;
using lumina::imaging::ImageBuf;
using lumina::json::JsonValue;
using lumina::plan::OpTable;

static int g_fail = 0;
#define CHECK(cond, msg)                                             \
  do {                                                               \
    if (!(cond)) {                                                   \
      std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, msg);       \
      ++g_fail;                                                      \
    } else {                                                         \
      std::printf("  [PASS] %s\n", msg);                             \
    }                                                                \
  } while (0)

namespace {

// 单算子施加（每次重新注入参数；OpTable 的 setParams 有状态）
bool runOne(const std::string& op, const JsonValue& params, ImageBuf* buf,
            std::string* err) {
  std::vector<std::string> names{op};
  std::map<std::string, JsonValue> byOp{{op, params}};
  return OpTable::instance().evaluate(names, buf, byOp, err);
}

// max|Δ|（8bit LSB 尺度 = 码差/255）
double maxAbsDeltaLsb(const ImageBuf& a, const ImageBuf& b) {
  if (a.w != b.w || a.h != b.h || a.data.size() != b.data.size()) return 1e9;
  double m = 0.0;
  for (size_t i = 0; i < a.data.size(); ++i) {
    const double d = std::fabs(static_cast<double>(a.data[i]) - static_cast<double>(b.data[i])) / 255.0;
    if (d > m) m = d;
  }
  return m;
}

double meanAbsDeltaLsb(const ImageBuf& a, const ImageBuf& b) {
  if (a.data.size() != b.data.size() || a.data.empty()) return 1e9;
  double sum = 0.0;
  for (size_t i = 0; i < a.data.size(); ++i) {
    sum += std::fabs(static_cast<double>(a.data[i]) - static_cast<double>(b.data[i]));
  }
  return sum / (static_cast<double>(a.data.size()) * 255.0);
}

// A1 夹具：亮度沿 x 斜坡 + 色相沿 y 旋转 + 细纹（luminance/colorRange/clarity 都有料）
ImageBuf makeColorFixture(int w, int h) {
  ImageBuf buf(w, h);
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      const double t = (w > 1) ? static_cast<double>(x) / (w - 1) : 0.0;
      const double s = (h > 1) ? static_cast<double>(y) / (h - 1) : 0.0;
      double L = 0.08 + 0.84 * t;
      const double hue = s;
      const double sat = 0.45 + 0.15 * std::sin(6.283185307 * t);
      L += ((x % 8) - 4) * 0.004 + ((y % 8) - 4) * 0.003;  // 细纹（clarity 有残差）
      if (L < 0.0) L = 0.0;
      if (L > 1.0) L = 1.0;
      double r = 0.0, g = 0.0, b = 0.0;
      lumina::plan::hslToRgb(hue, sat, L, &r, &g, &b);
      uint16_t* p = &buf.data[(static_cast<size_t>(y) * w + x) * 3];
      for (int c = 0; c < 3; ++c) {
        const double v = (c == 0 ? r : (c == 1 ? g : b)) * 65535.0;
        p[c] = static_cast<uint16_t>(v + 0.5);
      }
    }
  }
  return buf;
}

// ---- 独立参考实现（按 §3.1.4 公式表重写；不复用 mask.cpp 施加代码）----
namespace ref {

double smoothstep(double e0, double e1, double x) {
  if (e1 <= e0) return x >= e1 ? 1.0 : 0.0;
  double t = (x - e0) / (e1 - e0);
  if (t < 0.0) t = 0.0;
  if (t > 1.0) t = 1.0;
  return t * t * (3.0 - 2.0 * t);
}

double clamp01(double v) { return v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v); }

double factorLinear(const JsonValue& g, double u, double v) {
  const double x1 = g.get("x1").asDouble(0.0), y1 = g.get("y1").asDouble(0.0);
  const double x2 = g.get("x2").asDouble(1.0), y2 = g.get("y2").asDouble(1.0);
  const double dx = x2 - x1, dy = y2 - y1;
  const double len2 = dx * dx + dy * dy;
  if (len2 <= 1e-12) return 0.0;
  return clamp01(((u - x1) * dx + (v - y1) * dy) / len2);
}

double factorRadial(const JsonValue& g, double u, double v) {
  const double cx = g.get("cx").asDouble(0.5), cy = g.get("cy").asDouble(0.5);
  const double r = g.get("radius").asDouble(0.4);
  double ratio = g.get("ratio").asDouble(1.0);
  if (ratio <= 1e-9) ratio = 1.0;
  double feather = g.get("feather").asDouble(0.45);
  feather = clamp01(feather);
  const double dx = u - cx, dy = (v - cy) / ratio;
  const double d = std::sqrt(dx * dx + dy * dy);
  return 1.0 - smoothstep(r * (1.0 - feather), r, d);
}

double factorLumRange(const JsonValue& rg, double L) {
  const double low = rg.get("low").asDouble(0.0);
  const double high = rg.get("high").asDouble(1.0);
  const double s = clamp01(rg.get("smooth").asDouble(0.1));
  return smoothstep(low - s, low, L) * (1.0 - smoothstep(high, high + s, L));
}

double factorColorRange(const JsonValue& rg, double r, double g, double b) {
  const double hc = rg.get("hueCenter").asDouble(0.0);
  const double hw = rg.get("hueWidth").asDouble(0.1);
  const double sl = rg.get("satLow").asDouble(0.0);
  const double sh = rg.get("satHigh").asDouble(1.0);
  const double s = clamp01(rg.get("smooth").asDouble(0.1));
  double h = 0.0, sat = 0.0, l = 0.0;
  lumina::plan::rgbToHsl(clamp01(r), clamp01(g), clamp01(b), &h, &sat, &l);
  double dh = h - hc;
  dh -= std::floor(dh + 0.5);
  const double half = std::max(0.0, hw) * 0.5;
  // 注意参数序：ref::smoothstep(e0, e1, x)；窗口 = [half*(1-s), half]，求值点 = |dh|。
  const double fHue = 1.0 - smoothstep(half * (1.0 - s), half, std::fabs(dh));
  const double fSat = smoothstep(sl, sl + s, sat) * (1.0 - smoothstep(sh, sh + s, sat));
  return fHue * fSat;
}

// 画笔场采样：场生成用镜像权威 buildBrushField256，双线性采样独立实现
double factorBrush(const JsonValue& strokes, double u, double v) {
  std::vector<float> field(256 * 256);
  lumina::plan::buildBrushField256(strokes, field.data());
  double fx = u * 256.0 - 0.5;
  double fy = v * 256.0 - 0.5;
  fx = std::max(0.0, std::min(255.0, fx));
  fy = std::max(0.0, std::min(255.0, fy));
  const int x0 = static_cast<int>(std::floor(fx));
  const int y0 = static_cast<int>(std::floor(fy));
  const double tx = fx - x0, ty = fy - y0;
  const int x1 = std::min(255, x0 + 1), y1 = std::min(255, y0 + 1);
  const double a = field[static_cast<size_t>(y0) * 256 + x0];
  const double b = field[static_cast<size_t>(y0) * 256 + x1];
  const double c = field[static_cast<size_t>(y1) * 256 + x0];
  const double d = field[static_cast<size_t>(y1) * 256 + x1];
  return (a * (1 - tx) + b * tx) * (1 - ty) + (c * (1 - tx) + d * tx) * ty;
}

// 9 叶局部链（§3.1.4；与实现同序同 clamp 点，但独立誊写公式）
void applyLocal(double u[3], double w, const JsonValue& local) {
  const double E = local.get("exposure").asDouble(0.0);
  const double C = local.get("contrast").asDouble(0.0);
  const double H = local.get("highlights").asDouble(0.0);
  const double Sh = local.get("shadows").asDouble(0.0);
  const double T = local.get("temperature").asDouble(0.0);
  const double Ti = local.get("tint").asDouble(0.0);
  const double Sat = local.get("saturation").asDouble(0.0);
  const double Cl = local.get("clarity").asDouble(0.0);
  const double Dz = local.get("dehaze").asDouble(0.0);
  (void)Cl;  // clarity 需要共享模糊平面，A1 参考走带 clarity 的专属路径（见 refApply）
  if (E != 0.0) {
    const double g = std::pow(2.0, E * w);
    u[0] *= g;
    u[1] *= g;
    u[2] *= g;
  }
  if (C != 0.0) {
    const double k = 1.0 + C / 100.0 * 0.5 * w;
    for (int c = 0; c < 3; ++c) u[c] = (u[c] - 0.5) * k + 0.5;
  }
  if (H != 0.0) {
    const double L = 0.2126 * u[0] + 0.7152 * u[1] + 0.0722 * u[2];
    const double k = H / 100.0 * 0.4 * w * smoothstep(0.45, 1.0, L);
    for (int c = 0; c < 3; ++c) u[c] += k * (1.0 - u[c]);
  }
  if (Sh != 0.0) {
    const double L = 0.2126 * u[0] + 0.7152 * u[1] + 0.0722 * u[2];
    const double k = Sh / 100.0 * 0.4 * w * (1.0 - smoothstep(0.0, 0.55, L));
    for (int c = 0; c < 3; ++c) u[c] += k * (1.0 - u[c]);
  }
  if (T != 0.0) {
    const double g = T / 100.0 * 0.10 * w;
    u[0] *= std::pow(2.0, g);
    u[2] *= std::pow(2.0, -g);
  }
  if (Ti != 0.0) u[1] *= std::pow(2.0, Ti / 100.0 * 0.05 * w);
  if (Sat != 0.0) {
    for (int c = 0; c < 3; ++c) u[c] = clamp01(u[c]);
    double h = 0.0, s = 0.0, l = 0.0;
    lumina::plan::rgbToHsl(u[0], u[1], u[2], &h, &s, &l);
    s = clamp01(s * (1.0 + Sat / 100.0 * 0.6 * w));
    double r = 0.0, g2 = 0.0, b = 0.0;
    lumina::plan::hslToRgb(h, s, l, &r, &g2, &b);
    u[0] = r;
    u[1] = g2;
    u[2] = b;
  }
  if (Dz != 0.0) {
    const double d = Dz / 100.0 * w;
    for (int c = 0; c < 3; ++c) {
      u[c] = d >= 0.0 ? (u[c] - 0.15 * d) / (1.0 - 0.15 * d)
                      : u[c] * (1.0 + 0.3 * d) + 0.10 * (-d);
    }
  }
}

}  // namespace ref

// A1：单蒙版端到端参考（含 clarity 的共享模糊路径）。返回参考 uint16 缓冲。
ImageBuf refApplySingleMask(const JsonValue& maskJson, const ImageBuf& base) {
  const JsonValue& local = maskJson.get("local");
  const std::string type = maskJson.get("type").asString();
  const JsonValue geometry = maskJson.get("geometry");
  const JsonValue range = maskJson.get("range");
  const JsonValue strokes = maskJson.get("strokes");
  const double opacity = maskJson.get("opacity").asDouble(100.0) / 100.0;
  const double amount = maskJson.get("amount").asDouble(100.0) / 100.0;

  // clarity 参考模糊平面（共享，码域；与实现同语义的独立誊写：盒均值 = boxBlurPlane）
  const double Cl = local.get("clarity").asDouble(0.0);
  std::vector<float> blur;
  const int W = base.w, H = base.h;
  if (Cl != 0.0) {
    blur.resize(static_cast<size_t>(W) * H * 3);
    std::vector<float> src(static_cast<size_t>(W) * H), dst(static_cast<size_t>(W) * H);
    for (int c = 0; c < 3; ++c) {
      for (size_t p = 0; p < src.size(); ++p) src[p] = imaging::codeAt(base, p * 3 + c);
      lumina::plan::boxBlurPlane(src.data(), dst.data(), W, H, 10);
      std::copy(dst.begin(), dst.end(), blur.begin() + static_cast<size_t>(c) * W * H);
    }
  }

  ImageBuf out = base;
  for (int y = 0; y < H; ++y) {
    const double v = (static_cast<double>(y) + 0.5) / H;
    for (int x = 0; x < W; ++x) {
      const double u = (static_cast<double>(x) + 0.5) / W;
      double f = 0.0;
      if (type == "linear") {
        f = ref::factorLinear(geometry, u, v);
      } else if (type == "radial") {
        f = ref::factorRadial(geometry, u, v);
      } else if (type == "brush") {
        f = ref::factorBrush(strokes, u, v);
      } else if (type == "luminanceRange") {
        const size_t i0 = (static_cast<size_t>(y) * W + x) * 3;
        const double L = 0.2126 * (imaging::codeAt(base, i0) / 65535.0) +
                         0.7152 * (imaging::codeAt(base, i0 + 1) / 65535.0) +
                         0.0722 * (imaging::codeAt(base, i0 + 2) / 65535.0);
        f = ref::factorLumRange(range, L);
      } else if (type == "colorRange") {
        const size_t i0 = (static_cast<size_t>(y) * W + x) * 3;
        f = ref::factorColorRange(range, imaging::codeAt(base, i0) / 65535.0,
                                  imaging::codeAt(base, i0 + 1) / 65535.0,
                                  imaging::codeAt(base, i0 + 2) / 65535.0);
      }
      double w = f * opacity * amount;
      w = std::max(-1.0, std::min(1.0, w));
      if (w == 0.0) continue;
      const size_t i0 = (static_cast<size_t>(y) * W + x) * 3;
      double u3[3] = {imaging::codeAt(base, i0) / 65535.0,
                      imaging::codeAt(base, i0 + 1) / 65535.0,
                      imaging::codeAt(base, i0 + 2) / 65535.0};
      ref::applyLocal(u3, w, local);
      if (Cl != 0.0 && !blur.empty()) {
        const double k = Cl / 100.0 * 0.6 * w;
        for (int c = 0; c < 3; ++c) {
          const double B = blur[static_cast<size_t>(c) * W * H +
                                static_cast<size_t>(y) * W + x] / 65535.0;
          u3[c] += k * (u3[c] - B);
        }
      }
      for (int c = 0; c < 3; ++c) {
        double code = u3[c] * 65535.0;
        if (code < 0.0) code = 0.0;
        if (code > 65535.0) code = 65535.0;
        out.data[i0 + c] = static_cast<uint16_t>(code + 0.5);
      }
    }
  }
  return out;
}

// ---- A1：5 类 × 3 组 vs 浮点参考 ----
JsonValue makeMaskJson(const char* type, const JsonValue& extra,
                       const JsonValue& local) {
  JsonValue m = JsonValue::objectValue();
  m.setItem("id", "m1");
  m.setItem("type", type);
  m.setItem("enabled", true);
  m.setItem("opacity", 100);
  m.setItem("amount", 100);
  m.setItem("combine", "replace");
  // 把 extra 放进与 type 对应的子键：brush→strokes、luminanceRange/colorRange→range、
  // linear/radial→geometry。parseMaskSpec 与 refApplySingleMask 均按此子键读取（§3.1.1）。
  const std::string t(type);
  if (t == "brush") {
    if (!extra.isNull()) m.setItem("strokes", extra);
  } else if (t == "luminanceRange" || t == "colorRange") {
    if (!extra.isNull()) m.setItem("range", extra);
  } else {
    if (!extra.isNull()) m.setItem("geometry", extra);
  }
  m.setItem("local", local);
  return m;
}

JsonValue localG(int gi) {
  JsonValue l = JsonValue::objectValue();
  switch (gi) {
    case 0:
      l.setItem("exposure", 1.0);
      break;
    case 1:
      l.setItem("temperature", 60.0);
      l.setItem("tint", -30.0);
      l.setItem("saturation", 40.0);
      break;
    default:
      l.setItem("contrast", 70.0);
      l.setItem("highlights", -60.0);
      l.setItem("shadows", 40.0);
      l.setItem("clarity", 50.0);
      l.setItem("dehaze", -35.0);
      break;
  }
  return l;
}

struct TypeCase {
  const char* type;
  JsonValue extra;
};

std::vector<TypeCase> typeCases() {
  JsonValue linearGeo = JsonValue::objectValue();
  linearGeo.setItem("x1", 0.1);
  linearGeo.setItem("y1", 0.1);
  linearGeo.setItem("x2", 0.9);
  linearGeo.setItem("y2", 0.7);
  JsonValue radialGeo = JsonValue::objectValue();
  radialGeo.setItem("cx", 0.4);
  radialGeo.setItem("cy", 0.55);
  radialGeo.setItem("radius", 0.35);
  radialGeo.setItem("ratio", 1.6);
  radialGeo.setItem("feather", 0.5);
  JsonValue strokes = JsonValue::arrayValue();
  {
    JsonValue s1 = JsonValue::objectValue();
    s1.setItem("x", 0.3);
    s1.setItem("y", 0.4);
    s1.setItem("radius", 0.25);
    s1.setItem("flow", 100);
    s1.setItem("density", 100);
    s1.setItem("erase", false);
    strokes.addItem(s1);
    JsonValue s2 = JsonValue::objectValue();
    s2.setItem("x", 0.6);
    s2.setItem("y", 0.6);
    s2.setItem("radius", 0.2);
    s2.setItem("flow", 80);
    s2.setItem("density", 100);
    s2.setItem("erase", false);
    strokes.addItem(s2);
  }
  JsonValue lumRange = JsonValue::objectValue();
  lumRange.setItem("low", 0.25);
  lumRange.setItem("high", 0.75);
  lumRange.setItem("smooth", 0.12);
  JsonValue colorRange = JsonValue::objectValue();
  colorRange.setItem("hueCenter", 0.0);
  colorRange.setItem("hueWidth", 0.12);
  colorRange.setItem("satLow", 0.05);
  colorRange.setItem("satHigh", 0.9);
  colorRange.setItem("smooth", 0.1);
  return {
      {"linear", linearGeo},      {"radial", radialGeo},   {"brush", strokes},
      {"luminanceRange", lumRange}, {"colorRange", colorRange},
  };
}

void testA1AgainstReference() {
  std::printf("A1 单蒙版 5 类 x 3 组 vs 独立双精度参考（mean<=2/255, max<=6/255）\n");
  const ImageBuf base = makeColorFixture(160, 120);
  const auto types = typeCases();
  for (const TypeCase& tc : types) {
    for (int gi = 0; gi < 3; ++gi) {
      JsonValue m = makeMaskJson(tc.type, tc.extra, localG(gi));
      JsonValue params = JsonValue::objectValue();
      JsonValue arr = JsonValue::arrayValue();
      arr.addItem(m);
      params.setItem("masks", arr);
      ImageBuf got = base;
      std::string err;
      const bool ok = runOne("mask.apply", params, &got, &err);
      if (!ok) {
        CHECK(false, (std::string("A1 ") + tc.type + " G" + std::to_string(gi) +
                      " 执行失败: " + err).c_str());
        continue;
      }
      const ImageBuf want = refApplySingleMask(m, base);
      const double mean = meanAbsDeltaLsb(got, want);
      const double mx = maxAbsDeltaLsb(got, want);
      char buf[160];
      std::snprintf(buf, sizeof(buf), "%s G%d: mean|Δ|=%.4f max|Δ|=%.4f", tc.type, gi, mean, mx);
      std::printf("    %s\n", buf);
      CHECK(mean <= 2.0 && mx <= 6.0,
            (std::string("A1 ") + tc.type + " G" + std::to_string(gi) +
             " mean<=2/255 且 max<=6/255").c_str());
    }
  }
}

// ---- A2：区外 ≤1 LSB、区内 ±1EV ≥8/255 ----
void testA2InsideOutside() {
  std::printf("A2 蒙版区内外分区口径（radial feather=0, exposure +1EV）\n");
  const int W = 160, H = 120;
  ImageBuf base(W, H);
  for (size_t i = 0; i < base.data.size(); ++i) base.data[i] = static_cast<uint16_t>(0.4 * 65535.0 + 0.5);

  JsonValue geo = JsonValue::objectValue();
  geo.setItem("cx", 0.5);
  geo.setItem("cy", 0.5);
  geo.setItem("radius", 0.35);
  geo.setItem("ratio", 1.0);
  geo.setItem("feather", 0.0);
  JsonValue local = JsonValue::objectValue();
  local.setItem("exposure", 1.0);
  JsonValue m = makeMaskJson("radial", geo, local);
  JsonValue params = JsonValue::objectValue();
  JsonValue arr = JsonValue::arrayValue();
  arr.addItem(m);
  params.setItem("masks", arr);
  ImageBuf got = base;
  std::string err;
  CHECK(runOne("mask.apply", params, &got, &err), "A2 mask.apply 执行成功");
  if (g_fail) return;

  double minInsideLsb = 1e9;
  double maxOutsideLsb = 0.0;
  int insideCount = 0;
  for (int y = 0; y < H; ++y) {
    for (int x = 0; x < W; ++x) {
      const double u = (x + 0.5) / W;
      const double v = (y + 0.5) / H;
      const double d = std::sqrt((u - 0.5) * (u - 0.5) + (v - 0.5) * (v - 0.5));
      double deltaLsb = 0.0;
      for (int c = 0; c < 3; ++c) {
        const size_t i0 = (static_cast<size_t>(y) * W + x) * 3 + c;
        deltaLsb = std::max(deltaLsb,
                            std::fabs(static_cast<double>(got.data[i0]) -
                                      static_cast<double>(base.data[i0])) / 255.0);
      }
      if (d <= 0.30) {  // 区内（远离羽化边）
        if (deltaLsb < minInsideLsb) minInsideLsb = deltaLsb;
        ++insideCount;
      } else if (d >= 0.40) {  // 区外（半径之外，raw 严格为 0）
        if (deltaLsb > maxOutsideLsb) maxOutsideLsb = deltaLsb;
      }
    }
  }
  std::printf("    inside n=%d min|Δ|=%.2f LSB；outside max|Δ|=%.4f LSB\n", insideCount,
              minInsideLsb, maxOutsideLsb);
  CHECK(insideCount > 0 && minInsideLsb >= 8.0, "区内 ±1EV ≥8/255");
  CHECK(maxOutsideLsb <= 1.0, "区外 ≤1 LSB");
}

// ---- 羽化边界抽样证据 ----
void testFeatherBoundary() {
  std::printf("羽化边界抽样（radial feather=0.5 + brush 场端点）\n");
  const int W = 200, H = 200;
  ImageBuf base(W, H);
  for (size_t i = 0; i < base.data.size(); ++i) base.data[i] = 32768;

  JsonValue geo = JsonValue::objectValue();
  geo.setItem("cx", 0.5);
  geo.setItem("cy", 0.5);
  geo.setItem("radius", 0.4);
  geo.setItem("ratio", 1.0);
  geo.setItem("feather", 0.5);
  JsonValue m = makeMaskJson("radial", geo, JsonValue::objectValue());
  lumina::plan::MaskSpec spec;
  std::string perr;
  CHECK(lumina::plan::parseMaskSpec(m, &spec, &perr), "radial MaskSpec 解析成功");
  // 沿 y=100 扫描线抽样：因子须与参考公式一致；单调不增只在中心右侧一支成立
  // （水平扫描线穿过圆心，f 沿线呈 ∩ 形：0→1→0，故取 x≥W/2 径向向外的半支验证单调）。
  bool allMatch = true;
  bool monotonic = true;
  double prev = 2.0;
  for (int x = 0; x < W; ++x) {
    double f = -1.0;
    std::string err;
    if (!lumina::plan::maskFactorAt(spec, base, x, 100, &f, &err)) {
      CHECK(false, "maskFactorAt radial 失败");
      return;
    }
    const double u = (x + 0.5) / W;
    const double v = (100 + 0.5) / H;
    const double d = std::sqrt((u - 0.5) * (u - 0.5) + (v - 0.5) * (v - 0.5));
    const double expect = 1.0 - ref::smoothstep(0.4 * 0.5, 0.4, d);
    if (std::fabs(f - expect) > 1e-9) allMatch = false;
    if (x >= W / 2) {  // 径向向外半支：d 随 x 严格增，f 单调不增
      if (f > prev + 1e-12) monotonic = false;
      prev = f;
    }
  }
  CHECK(allMatch, "radial feather 场逐点 = 1 - smoothstep(0.2r 起点, r, d)");
  CHECK(monotonic, "radial 因子沿径向单调不增");
  double fc = -1.0, fe = -1.0, fm = -1.0;
  std::string err;
  lumina::plan::maskFactorAt(spec, base, 100, 100, &fc, &err);  // 中心 d≈0 → 1
  lumina::plan::maskFactorAt(spec, base, 180, 100, &fe, &err);  // d≈0.4025 ≥ r → 0
  lumina::plan::maskFactorAt(spec, base, 159, 100, &fm, &err);  // d≈0.2975 → 羽化中段
  std::printf("    f(center)=%.6f f(mid)=%.6f f(outside)=%.6f\n", fc, fm, fe);
  CHECK(fc > 0.999, "羽化中心 f=1");
  CHECK(fm > 0.1 && fm < 0.9, "羽化中段因子落在过渡带（0.1<f<0.9）");
  CHECK(fe == 0.0, "羽化外 f=0");

  // brush 场：单印记中心 = flow/100（8bit 量化），半径外 = 0
  JsonValue strokes = JsonValue::arrayValue();
  JsonValue s1 = JsonValue::objectValue();
  s1.setItem("x", 0.5);
  s1.setItem("y", 0.5);
  s1.setItem("radius", 0.3);
  s1.setItem("flow", 70);
  s1.setItem("density", 100);
  s1.setItem("erase", false);
  strokes.addItem(s1);
  std::vector<float> field(256 * 256);
  lumina::plan::buildBrushField256(strokes, field.data());
  const double centerVal = field[128 * 256 + 128];  // nx=ny≈0.502, d < r/2 → 全强度
  const double cornerVal = field[0];                // d≈0.704 > r → 0
  std::printf("    brush f(center)=%.6f（期望 179/255=%.6f） f(corner)=%.6f\n", centerVal,
              179.0 / 255.0, cornerVal);
  CHECK(std::fabs(centerVal - 179.0 / 255.0) < 1e-6, "brush 中心 = flow/100（8bit 量化口径）");
  CHECK(cornerVal == 0.0, "brush 半径外 = 0");
}

// ---- A4：20 槽逐个关闭均产生差异 + 全 replace 置换不变 ----
JsonValue makeGridMask(int i, bool enabled) {
  const int col = i % 5;
  const int row = i / 5;
  JsonValue geo = JsonValue::objectValue();
  geo.setItem("cx", (col + 0.5) / 5.0);
  geo.setItem("cy", (row + 0.5) / 4.0);
  geo.setItem("radius", 0.09);  // 5 列间距 0.2、4 行间距 0.25 → 硬边互不重叠
  geo.setItem("ratio", 1.0);
  geo.setItem("feather", 0.0);
  JsonValue local = JsonValue::objectValue();
  local.setItem("exposure", 0.4 + i * 0.02);
  JsonValue m = makeMaskJson("radial", geo, local);
  m.setItem("id", "m" + std::to_string(i));
  m.setItem("enabled", enabled);
  return m;
}

bool applyGrid(const std::vector<JsonValue>& masks, const ImageBuf& base, ImageBuf* out,
               std::string* err) {
  JsonValue params = JsonValue::objectValue();
  JsonValue arr = JsonValue::arrayValue();
  for (const JsonValue& m : masks) arr.addItem(m);
  params.setItem("masks", arr);
  *out = base;
  return runOne("mask.apply", params, out, err);
}

void testA4TwentySlots() {
  std::printf("A4 20 槽逐个关闭均产生差异 + 全 replace 置换数组序不变\n");
  ImageBuf base(200, 150);
  for (size_t i = 0; i < base.data.size(); ++i) base.data[i] = static_cast<uint16_t>(0.35 * 65535.0 + 0.5);
  std::vector<JsonValue> masks;
  for (int i = 0; i < 20; ++i) masks.push_back(makeGridMask(i, true));
  ImageBuf full;
  std::string err;
  CHECK(applyGrid(masks, base, &full, &err), "A4 全 20 槽执行成功");
  if (g_fail) return;
  bool allDiffer = true;
  double minDelta = 1e9;
  for (int k = 0; k < 20; ++k) {
    std::vector<JsonValue> without = masks;
    without[k].setItem("enabled", false);
    ImageBuf got;
    if (!applyGrid(without, base, &got, &err)) {
      CHECK(false, (std::string("A4 关闭槽 ") + std::to_string(k) + " 执行失败").c_str());
      return;
    }
    const double d = maxAbsDeltaLsb(full, got);
    if (d < minDelta) minDelta = d;
    if (d <= 0.0) allDiffer = false;
  }
  std::printf("    逐槽关闭的最小 max|Δ| = %.2f LSB\n", minDelta);
  CHECK(allDiffer, "20 槽逐个关闭均产生差异（min max|Δ| > 0）");

  // 置换不变：硬边互不重叠 + 全 replace → 每像素至多被一个蒙版触碰，
  // 数组序不影响结果（fold 只依赖数组序+combine 的顺序无关键）
  std::vector<JsonValue> reversed = masks;
  std::reverse(reversed.begin(), reversed.end());
  ImageBuf perm;
  CHECK(applyGrid(reversed, base, &perm, &err), "A4 置换序执行成功");
  const double pd = maxAbsDeltaLsb(full, perm);
  std::printf("    置换序 max|Δ| = %.4f LSB\n", pd);
  CHECK(pd == 0.0, "全 replace 硬边置换数组序逐字节不变");
}

// ---- A7：分区口径方向 ----
double meanLum(const ImageBuf& b, int x0, int x1, int y0, int y1) {
  double sum = 0.0;
  int n = 0;
  for (int y = y0; y < y1; ++y) {
    for (int x = x0; x < x1; ++x) {
      const size_t i0 = (static_cast<size_t>(y) * b.w + x) * 3;
      sum += 0.2126 * (imaging::codeAt(b, i0) / 65535.0) +
             0.7152 * (imaging::codeAt(b, i0 + 1) / 65535.0) +
             0.0722 * (imaging::codeAt(b, i0 + 2) / 65535.0);
      ++n;
    }
  }
  return n > 0 ? sum / n : 0.0;
}

void testA7Direction() {
  std::printf("A7 分区口径方向（linear 梯度 / temperature / highlights）\n");
  const ImageBuf base = makeColorFixture(160, 120);
  std::string err;

  // ① linear 梯度方向：(x1,y1)=0 → (x2,y2)=1，右半增量 > 左半增量
  {
    JsonValue geo = JsonValue::objectValue();
    geo.setItem("x1", 0.05);
    geo.setItem("y1", 0.5);
    geo.setItem("x2", 0.95);
    geo.setItem("y2", 0.5);
    JsonValue local = JsonValue::objectValue();
    local.setItem("exposure", 0.8);
    JsonValue m = makeMaskJson("linear", geo, local);
    JsonValue params = JsonValue::objectValue();
    JsonValue arr = JsonValue::arrayValue();
    arr.addItem(m);
    params.setItem("masks", arr);
    ImageBuf got = base;
    CHECK(runOne("mask.apply", params, &got, &err), "A7 linear 执行成功");
    const double left = meanLum(got, 0, 40, 0, 120) - meanLum(base, 0, 40, 0, 120);
    const double right = meanLum(got, 120, 160, 0, 120) - meanLum(base, 120, 160, 0, 120);
    std::printf("    linear 左半 Δlum=%+.5f 右半 Δlum=%+.5f\n", left, right);
    CHECK(right > left + 0.02 && right > 0.0, "linear 梯度方向正确（右半增量 > 左半增量）");
  }
  // ② temperature 方向：+T → R 升 B 降
  {
    JsonValue geo = JsonValue::objectValue();
    geo.setItem("cx", 0.5);
    geo.setItem("cy", 0.5);
    geo.setItem("radius", 0.35);
    geo.setItem("ratio", 1.0);
    geo.setItem("feather", 0.0);
    JsonValue local = JsonValue::objectValue();
    local.setItem("temperature", 60.0);
    JsonValue m = makeMaskJson("radial", geo, local);
    JsonValue params = JsonValue::objectValue();
    JsonValue arr = JsonValue::arrayValue();
    arr.addItem(m);
    params.setItem("masks", arr);
    ImageBuf got = base;
    CHECK(runOne("mask.apply", params, &got, &err), "A7 temperature 执行成功");
    double dR = 0.0, dB = 0.0;
    int n = 0;
    for (int y = 0; y < 120; ++y) {
      for (int x = 0; x < 160; ++x) {
        const double u = (x + 0.5) / 160, v = (y + 0.5) / 120;
        if (std::sqrt((u - 0.5) * (u - 0.5) + (v - 0.5) * (v - 0.5)) > 0.3) continue;
        const size_t i0 = (static_cast<size_t>(y) * 160 + x) * 3;
        dR += static_cast<double>(got.data[i0]) - base.data[i0];
        dB += static_cast<double>(got.data[i0 + 2]) - base.data[i0 + 2];
        ++n;
      }
    }
    dR /= n;
    dB /= n;
    std::printf("    temperature 区内 meanΔR=%+.1f code meanΔB=%+.1f code\n", dR, dB);
    CHECK(dR > 0.0 && dB < 0.0, "temperature 方向正确（R 升 B 降）");
  }
  // ③ highlights 方向：-H 在亮区效应强于暗区
  {
    ImageBuf ramp(160, 120);
    for (int y = 0; y < 120; ++y) {
      for (int x = 0; x < 160; ++x) {
        const double t = x / 159.0;
        const uint16_t c = static_cast<uint16_t>((0.10 + 0.80 * t) * 65535.0 + 0.5);
        for (int cc = 0; cc < 3; ++cc) ramp.data[(static_cast<size_t>(y) * 160 + x) * 3 + cc] = c;
      }
    }
    JsonValue geo = JsonValue::objectValue();
    geo.setItem("cx", 0.5);
    geo.setItem("cy", 0.5);
    geo.setItem("radius", 0.45);
    geo.setItem("ratio", 1.0);
    geo.setItem("feather", 0.0);
    JsonValue local = JsonValue::objectValue();
    local.setItem("highlights", -60.0);
    JsonValue m = makeMaskJson("radial", geo, local);
    JsonValue params = JsonValue::objectValue();
    JsonValue arr = JsonValue::arrayValue();
    arr.addItem(m);
    params.setItem("masks", arr);
    ImageBuf got = ramp;
    CHECK(runOne("mask.apply", params, &got, &err), "A7 highlights 执行成功");
    const double dark = meanLum(got, 0, 40, 0, 120) - meanLum(ramp, 0, 40, 0, 120);
    const double bright = meanLum(got, 120, 160, 0, 120) - meanLum(ramp, 120, 160, 0, 120);
    std::printf("    highlights 暗端 Δlum=%+.5f 亮端 Δlum=%+.5f\n", dark, bright);
    CHECK(std::fabs(bright) > std::fabs(dark) * 2.0 && bright < 0.0,
          "highlights 方向正确（亮区压制强于暗区且为负）");
  }
}

}  // namespace

int main() {
  std::string err;
  if (!lumina::plan::registerBuiltinOps(&lumina::plan::OpTable::instance(), &err)) {
    std::printf("FATAL registerBuiltinOps: %s\n", err.c_str());
    return 2;
  }
  testA1AgainstReference();
  testA2InsideOutside();
  testFeatherBoundary();
  testA4TwentySlots();
  testA7Direction();
  std::printf("\n%s (%d failures)\n", g_fail == 0 ? "ALL PASS" : "FAILED", g_fail);
  return g_fail == 0 ? 0 : 1;
}
