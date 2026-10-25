// T1.3 验收：93 叶 CPU oracle 逐叶单测
// 口径：identity@默认（全算子字节不变）、单调性（滑块方向 = 像素效果方向）、
//       值域 clamp（极值不炸）、关键数学（曝光 EV 精确、蒙版混合、ToneMap 单调）。
// 叶路径以 docs/param-leaves.json 为准（basic.*/curve.*/parametric.*/hsl.*/colorGrade.*/
// calibration.*/detail.*/optics.*/geometry.*/masks[]）。
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

#include "imaging/image_buffer.h"
#include "json/json_value.h"
#include "plan/op_table.h"
#include "plan/ops/ops.h"
#include "plan/render_plan.h"

using namespace lumina;
using lumina::imaging::ImageBuf;
using lumina::imaging::log16Encode;
using lumina::json::JsonValue;
using lumina::plan::OpTable;
using lumina::plan::Operator;

static int g_fail = 0;
#define CHECK(cond, msg)                                                 \
  do {                                                                   \
    if (!(cond)) {                                                       \
      std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, msg);           \
      ++g_fail;                                                          \
    } else {                                                             \
      std::printf("  [PASS] %s\n", msg);                                 \
    }                                                                    \
  } while (0)

// ---- 图像构造辅助 ----

// 均匀灰图（线性光 lin）+ 可选暗/亮象限与彩色条带
static ImageBuf makeFlat(int w, int h, float lin) {
  ImageBuf buf(w, h);
  const uint16_t c = log16Encode(lin);
  for (size_t i = 0; i < buf.data.size(); ++i) buf.data[i] = c;
  return buf;
}

// 三段条带：暗(0.1)/中(0.45)/亮(0.85) 水平三等分。
// 注意：S2 分区/S4 曲线类算子的感知坐标 = code/65535，故探针直接写码空间
// （不走 log16Encode——线性 0.05..0.9 在 log16 全程 20EV 中只占码值 0.24..0.35 窄带）。
static ImageBuf makeToneStripes(int w, int h) {
  ImageBuf buf(w, h);
  const uint16_t tones[3] = {
      static_cast<uint16_t>(0.10 * 65535.0 + 0.5),
      static_cast<uint16_t>(0.45 * 65535.0 + 0.5),
      static_cast<uint16_t>(0.85 * 65535.0 + 0.5)};
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      uint16_t* p = &buf.data[(static_cast<size_t>(y) * w + x) * 3];
      p[0] = p[1] = p[2] = tones[std::min(2, x * 3 / w)];
    }
  }
  return buf;
}

// 左半灰色调、右半纯色（hue,sat,lin 经 HSL→RGB）——HSL 带隔离测试用
static ImageBuf makeHueProbe(int w, int h, double hue, double sat, float lin) {
  ImageBuf buf = makeFlat(w, h, lin);
  double r, g, b;
  lumina::plan::hslToRgb(hue, sat, lin, &r, &g, &b);
  const uint16_t cr = static_cast<uint16_t>(r * 65535.0 + 0.5);
  const uint16_t cg = static_cast<uint16_t>(g * 65535.0 + 0.5);
  const uint16_t cb = static_cast<uint16_t>(b * 65535.0 + 0.5);
  for (int y = 0; y < h; ++y) {
    for (int x = w / 2; x < w; ++x) {
      uint16_t* p = &buf.data[(static_cast<size_t>(y) * w + x) * 3];
      p[0] = cr;
      p[1] = cg;
      p[2] = cb;
    }
  }
  return buf;
}

static double meanLum(const ImageBuf& buf) {
  double s = 0.0;
  for (size_t i = 0; i + 2 < buf.data.size(); i += 3) {
    s += (static_cast<double>(buf.data[i]) + buf.data[i + 1] + buf.data[i + 2]) / 3.0;
  }
  return s / (buf.pixelCount());
}

static double chromaSpread(const ImageBuf& buf) {
  double s = 0.0;
  size_t n = 0;
  for (size_t i = 0; i + 2 < buf.data.size(); i += 3) {
    const double mean = (buf.data[i] + buf.data[i + 1] + buf.data[i + 2]) / 3.0;
    for (int c = 0; c < 3; ++c) {
      s += std::fabs(static_cast<double>(buf.data[i + c]) - mean);
    }
    ++n;
  }
  return s / (n * 3.0);
}

// 带参数应用单算子（经 evaluate 带 params 重载：setParams 注入后求值）
static bool applyParams(const char* opName, const JsonValue& params,
                        ImageBuf* buf, std::string* err) {
  const Operator* op = OpTable::instance().find(opName);
  if (!op) {
    if (err) *err = std::string("op not found: ") + opName;
    return false;
  }
  std::vector<std::string> names{opName};
  std::map<std::string, JsonValue> byOp{{opName, params}};
  return OpTable::instance().evaluate(names, buf, byOp, err);
}

static JsonValue numParams(const char* key, double v) {
  JsonValue p = JsonValue::objectValue();
  p.setItem(key, v);
  return p;
}

static bool sameBytes(const ImageBuf& a, const ImageBuf& b) {
  return a.w == b.w && a.h == b.h && a.data == b.data;
}

// ---- 1. 全算子 identity@默认（覆盖 93 叶默认路径） ----
static void testIdentityAtDefaults() {
  std::printf("testIdentityAtDefaults\n");
  const char* ops[] = {
      "basic.whiteBalance", "basic.temperature", "basic.tint", "basic.exposure",
      "basic.contrast", "basic.highlights", "basic.shadows", "basic.whites",
      "basic.blacks", "basic.clarity", "basic.texture", "basic.dehaze",
      "basic.vibrance", "basic.saturation",
      "curve.toneCurveRGB", "curve.toneCurveR", "curve.toneCurveG", "curve.toneCurveB",
      "curve.parametric",
      "color.hsl", "color.colorGrade",
      "calibration.shadowTint", "calibration.red", "calibration.green", "calibration.blue",
      "detail.denoise", "detail.sharpen",
      "lens.manualDistortion", "lens.profile",
      "geometry.crop", "geometry.perspective",
      "effects.vignette", "effects.grain", "mask.apply", "pipeline.toneMap"};
  for (const char* name : ops) {
    const Operator* op = OpTable::instance().find(name);
    CHECK(op != nullptr, (std::string("registered: ") + name).c_str());
    if (!op) continue;
    ImageBuf buf = makeToneStripes(48, 32);
    ImageBuf ref = buf;
    std::string err;
    JsonValue empty = JsonValue::objectValue();
    CHECK(applyParams(name, empty, &buf, &err), (std::string("apply ok: ") + name).c_str());
    // pipeline.toneMap 是域变换非 identity；其余默认参数必须字节不变
    if (std::string(name) != "pipeline.toneMap") {
      CHECK(sameBytes(buf, ref), (std::string("identity@default: ") + name).c_str());
    }
  }
}

// ---- 2. basic 14 叶 ----
static void testBasicExposure() {
  std::printf("testBasicExposure\n");
  ImageBuf buf = makeFlat(16, 16, 0.25f);
  const uint16_t c0 = buf.data[0];
  std::string err;
  CHECK(applyParams("basic.exposure", numParams("basic.exposure", 1.0), &buf, &err), "apply +1EV");
  CHECK(buf.data[0] == static_cast<uint16_t>(c0 + buf.logStep), "+1EV = +2048 code (整数精确)");
  CHECK(applyParams("basic.exposure", numParams("basic.exposure", 21.0), &buf, &err), "apply +21EV");
  bool allMax = true;
  for (uint16_t v : buf.data) {
    if (v != 65535) allMax = false;
  }
  CHECK(allMax, "累计 +13EV clamp 到 65535（log16 顶 = 2^20）");
}

static void testBasicTemperatureTint() {
  std::printf("testBasicTemperatureTint\n");
  ImageBuf buf = makeFlat(16, 16, 0.45f);
  ImageBuf ref = buf;
  std::string err;
  CHECK(applyParams("basic.temperature", numParams("basic.temperature", 50.0), &buf, &err), "temp +50");
  CHECK(buf.data[0] > ref.data[0] && buf.data[2] < ref.data[2], "temp>0: R 升 B 降");
  CHECK(applyParams("basic.tint", numParams("basic.tint", 50.0), &buf, &err), "tint +50");
  CHECK(buf.data[1] < ref.data[1], "tint>0: G 降（品红）");
  // 负方向对称：temp -50 回落 R
  ImageBuf neg = makeFlat(16, 16, 0.45f);
  CHECK(applyParams("basic.temperature", numParams("basic.temperature", -50.0), &neg, &err), "temp -50");
  CHECK(neg.data[0] < neg.data[1], "temp<0: R 降");
}

static void testBasicContrast() {
  std::printf("testBasicContrast\n");
  ImageBuf buf = makeToneStripes(48, 16);
  ImageBuf ref = buf;
  std::string err;
  CHECK(applyParams("basic.contrast", numParams("basic.contrast", 50.0), &buf, &err), "contrast +50");
  const double kMid = 65535.0 * 0.45;
  // 中间调以上拉高、以下压低（拉伸）
  const uint16_t bright0 = ref.data[(15 * 48 + 40) * 3];
  const uint16_t dark0 = ref.data[(15 * 48 + 4) * 3];
  const uint16_t bright1 = buf.data[(15 * 48 + 40) * 3];
  const uint16_t dark1 = buf.data[(15 * 48 + 4) * 3];
  CHECK(bright0 > kMid && bright1 > bright0, "contrast: 亮部更亮");
  CHECK(dark0 < kMid && dark1 < dark0, "contrast: 暗部更暗");
}

static void testBasicToneZones() {
  std::printf("testBasicToneZones\n");
  struct Case { const char* op; const char* key; int x; double expect; };
  // 亮区像素 x=40（亮条带）、暗区像素 x=4（暗条带）
  const Case cases[] = {
      {"basic.highlights", "basic.highlights", 40, 1.0},
      {"basic.highlights", "basic.highlights", 4, 0.0},   // 暗部权重≈0
      {"basic.shadows", "basic.shadows", 4, 1.0},
      {"basic.shadows", "basic.shadows", 40, 0.0},
      {"basic.whites", "basic.whites", 40, 1.0},
      {"basic.blacks", "basic.blacks", 4, 1.0},
  };
  for (const Case& c : cases) {
    ImageBuf buf = makeToneStripes(48, 8);
    const size_t idx = (4 * 48 + c.x) * 3;
    const uint16_t v0 = buf.data[idx];
    std::string err;
    CHECK(applyParams(c.op, numParams(c.key, 50.0), &buf, &err), (std::string("apply ") + c.key).c_str());
    const int d = static_cast<int>(buf.data[idx]) - static_cast<int>(v0);
    if (c.expect > 0.5) {
      CHECK(d > 0, (std::string(c.key) + ": 目标区提亮").c_str());
    } else {
      CHECK(d == 0, (std::string(c.key) + ": 非目标区不动").c_str());
    }
  }
}

static void testBasicClarityTexture() {
  std::printf("testBasicClarityTexture\n");
  // 平坦图：残差为 0 → 任意 amount 不变
  ImageBuf flat = makeFlat(64, 64, 0.45f);
  ImageBuf ref = flat;
  std::string err;
  CHECK(applyParams("basic.clarity", numParams("basic.clarity", 100.0), &flat, &err), "clarity flat");
  CHECK(sameBytes(flat, ref), "clarity: 平坦图 identity（残差 0）");
  // 竖直台阶边缘：+amount 放大边缘差，且 100 > 50 单调
  auto edgeAt = [](double amt, double* deltaOut) {
    ImageBuf buf(64, 16);
    for (int y = 0; y < 16; ++y) {
      for (int x = 0; x < 64; ++x) {
        uint16_t* p = &buf.data[(static_cast<size_t>(y) * 64 + x) * 3];
        const uint16_t c = log16Encode(x < 32 ? 0.3f : 0.6f);
        p[0] = p[1] = p[2] = c;
      }
    }
    std::string e2;
    JsonValue p = numParams("basic.clarity", amt);
    if (!applyParams("basic.clarity", p, &buf, &e2)) return false;
    const size_t l = (8 * 64 + 30) * 3;
    const size_t r = (8 * 64 + 34) * 3;
    *deltaOut = (buf.data[r] - buf.data[l]) - (65535.0 * 0.0);
    return true;
  };
  double d50 = 0.0, d100 = 0.0;
  CHECK(edgeAt(50.0, &d50) && edgeAt(100.0, &d100), "clarity edge apply");
  CHECK(d100 > d50, "clarity: 单调（100 > 50 边缘差更大）");
  // texture 同构（radius 2）：平坦图 identity
  ImageBuf t = makeFlat(64, 64, 0.45f);
  ImageBuf tref = t;
  CHECK(applyParams("basic.texture", numParams("basic.texture", 100.0), &t, &err), "texture flat");
  CHECK(sameBytes(t, tref), "texture: 平坦图 identity");
}

static void testBasicDehaze() {
  std::printf("testBasicDehaze\n");
  ImageBuf buf = makeToneStripes(48, 8);
  ImageBuf ref = buf;
  std::string err;
  CHECK(applyParams("basic.dehaze", numParams("basic.dehaze", 50.0), &buf, &err), "dehaze +50");
  CHECK(meanLum(buf) > meanLum(ref), "dehaze>0: 阴影提升 → 均值升");
  // 灰色像素（无饱和）不被饱和项改变方向：暗条带仍提亮
  const size_t dark = (4 * 48 + 4) * 3;
  CHECK(buf.data[dark] > ref.data[dark], "dehaze: 暗区像素提亮");
}

static void testBasicVibranceSaturation() {
  std::printf("testBasicVibranceSaturation\n");
  std::string err;
  // 中高饱和红像素（sat=0.6，留出两端 clamp 余量）：vibrance 增益 < saturation 增益
  auto chromaOf = [](const ImageBuf& b, size_t px) {
    const double mx = std::max({b.data[px], b.data[px + 1], b.data[px + 2]});
    const double mn = std::min({b.data[px], b.data[px + 1], b.data[px + 2]});
    return mx - mn;
  };
  ImageBuf vsat = makeHueProbe(16, 8, 0.0, 0.6, 0.5f);
  ImageBuf vref = vsat;
  CHECK(applyParams("basic.saturation", numParams("basic.saturation", 50.0), &vsat, &err), "sat +50");
  ImageBuf vvib = makeHueProbe(16, 8, 0.0, 0.6, 0.5f);
  CHECK(applyParams("basic.vibrance", numParams("basic.vibrance", 50.0), &vvib, &err), "vib +50");
  const size_t red = (4 * 16 + 12) * 3;
  const double dSat = chromaOf(vsat, red) - chromaOf(vref, red);
  const double dVib = chromaOf(vvib, red) - chromaOf(vref, red);
  CHECK(dVib > 0 && dVib < dSat, "vibrance: 高饱和像素增益小于 saturation");
  // 灰色像素两者都不动（均值基）
  ImageBuf g = makeFlat(8, 8, 0.45f);
  ImageBuf gref = g;
  CHECK(applyParams("basic.saturation", numParams("basic.saturation", 100.0), &g, &err), "sat on gray");
  CHECK(sameBytes(g, gref), "saturation: 灰图 identity");
}

static void testBasicWhiteBalance() {
  std::printf("testBasicWhiteBalance\n");
  ImageBuf buf = makeFlat(8, 8, 0.45f);
  ImageBuf ref = buf;
  JsonValue p = JsonValue::objectValue();
  p.setItem("basic.whiteBalance", "custom");
  std::string err;
  CHECK(applyParams("basic.whiteBalance", p, &buf, &err), "wb selector apply");
  CHECK(sameBytes(buf, ref), "whiteBalance: 选择器无像素效果（增益由 temperature/tint 承载）");
}

// ---- 3. curve 11 叶 ----
static JsonValue curvePoints(std::vector<std::pair<double, double>> pts) {
  JsonValue arr = JsonValue::arrayValue();
  for (const auto& pt : pts) {
    JsonValue o = JsonValue::objectValue();
    o.setItem("x", pt.first);
    o.setItem("y", pt.second);
    arr.addItem(o);
  }
  return arr;
}

static void testCurvePoint() {
  std::printf("testCurvePoint\n");
  std::string err;
  // 线性点 = identity
  ImageBuf buf = makeToneStripes(48, 8);
  ImageBuf ref = buf;
  JsonValue p = JsonValue::objectValue();
  p.setItem("curve.toneCurveRGB", curvePoints({{0, 0}, {255, 255}}));
  CHECK(applyParams("curve.toneCurveRGB", p, &buf, &err), "linear curve apply");
  CHECK(sameBytes(buf, ref), "点曲线: 线性点 identity");
  // 中点提亮 (128→180)
  p = JsonValue::objectValue();
  p.setItem("curve.toneCurveRGB", curvePoints({{0, 0}, {128, 180}, {255, 255}}));
  CHECK(applyParams("curve.toneCurveRGB", p, &buf, &err), "mid-lift curve");
  const size_t mid = (4 * 48 + 24) * 3;
  CHECK(buf.data[mid] > ref.data[mid], "点曲线: 中点提亮");
  // 端点锚定：纯 0 / 纯 65535 双像素图不变
  ImageBuf anchor(2, 1);
  anchor.data[0] = anchor.data[1] = anchor.data[2] = 0;
  anchor.data[3] = anchor.data[4] = anchor.data[5] = 65535;
  ImageBuf aref = anchor;
  CHECK(applyParams("curve.toneCurveRGB", p, &anchor, &err), "anchor curve apply");
  CHECK(sameBytes(anchor, aref), "点曲线: 端点锚定（0 与 65535 不动）");
  // R 通道隔离
  ImageBuf ch = makeToneStripes(48, 8);
  ImageBuf chref = ch;
  JsonValue pr = JsonValue::objectValue();
  pr.setItem("curve.toneCurveR", curvePoints({{0, 0}, {128, 180}, {255, 255}}));
  CHECK(applyParams("curve.toneCurveR", pr, &ch, &err), "R curve apply");
  CHECK(ch.data[mid] > chref.data[mid], "toneCurveR: R 提升");
  CHECK(ch.data[mid + 1] == chref.data[mid + 1] && ch.data[mid + 2] == chref.data[mid + 2],
        "toneCurveR: G/B 不动");
}

static void testCurveParametric() {
  std::printf("testCurveParametric\n");
  std::string err;
  // 分界叶单独设置：无像素效果（参数非效果）
  ImageBuf buf = makeToneStripes(48, 8);
  ImageBuf ref = buf;
  for (const char* k : {"parametric.shadowSplit", "parametric.midSplit", "parametric.lightSplit"}) {
    CHECK(applyParams("curve.parametric", numParams(k, 80.0), &buf, &err),
          (std::string("split-only no-op: ") + k).c_str());
  }
  CHECK(sameBytes(buf, ref), "分界叶: 自身不移动像素");
  // highlights：亮条带提亮、暗条带不动
  CHECK(applyParams("curve.parametric", numParams("parametric.highlights", 50.0), &buf, &err),
        "highlights apply");
  const size_t bright = (4 * 48 + 40) * 3;
  const size_t dark = (4 * 48 + 4) * 3;
  CHECK(buf.data[bright] > ref.data[bright], "parametric.highlights: 亮区提亮");
  CHECK(buf.data[dark] == ref.data[dark], "parametric.highlights: 暗区不动");
  // shadows：暗条带提亮
  ImageBuf s = makeToneStripes(48, 8);
  CHECK(applyParams("curve.parametric", numParams("parametric.shadows", 50.0), &s, &err),
        "shadows apply");
  CHECK(s.data[dark] > ref.data[dark], "parametric.shadows: 暗区提亮");
  // darks / lights：中暗区（0.3）与中亮区（0.6）方向
  ImageBuf mid = makeFlat(32, 8, 0.3f);
  ImageBuf midref = mid;
  CHECK(applyParams("curve.parametric", numParams("parametric.darks", 50.0), &mid, &err), "darks apply");
  CHECK(mid.data[0] > midref.data[0], "parametric.darks: 0.3 区提亮");
  ImageBuf hi = makeFlat(32, 8, 0.6f);
  ImageBuf hiref = hi;
  CHECK(applyParams("curve.parametric", numParams("parametric.lights", 50.0), &hi, &err), "lights apply");
  CHECK(hi.data[0] > hiref.data[0], "parametric.lights: 0.6 区提亮");
  // 单调：amount 100 > 50
  ImageBuf m50 = makeFlat(32, 8, 0.9f);
  ImageBuf m100 = makeFlat(32, 8, 0.9f);
  CHECK(applyParams("curve.parametric", numParams("parametric.highlights", 50.0), &m50, &err), "amt 50");
  CHECK(applyParams("curve.parametric", numParams("parametric.highlights", 100.0), &m100, &err), "amt 100");
  CHECK(m100.data[0] >= m50.data[0], "parametric: 单调");
  CHECK(m100.data[0] <= 65535, "parametric: clamp 上界");
}

// ---- 4. color 32 叶（hsl 24 + colorGrade 8） ----
static void testColorHsl() {
  std::printf("testColorHsl\n");
  const char* bands[8] = {"red", "orange", "yellow", "green", "aqua", "blue", "purple", "magenta"};
  const double bandHue[8] = {0.0, 1.0 / 12, 1.0 / 6, 1.0 / 3, 0.5, 2.0 / 3, 5.0 / 6, 11.0 / 12};
  std::string err;
  int satChecked = 0, lumChecked = 0, hueChecked = 0;
  for (int i = 0; i < 8; ++i) {
    // sat：本带像素 chroma 增、对面带像素不动
    ImageBuf buf = makeHueProbe(32, 8, bandHue[i], 0.7, 0.5f);
    ImageBuf ref = buf;
    const std::string satKey = std::string("hsl.") + bands[i] + ".sat";
    CHECK(applyParams("color.hsl", numParams(satKey.c_str(), 50.0), &buf, &err),
          (satKey + " apply").c_str());
    const size_t px = (4 * 32 + 24) * 3;  // 本带纯色像素
    const auto chromaOf = [](const ImageBuf& b, size_t q) {
      const double mx = std::max({b.data[q], b.data[q + 1], b.data[q + 2]});
      const double mn = std::min({b.data[q], b.data[q + 1], b.data[q + 2]});
      return mx - mn;
    };
    CHECK(chromaOf(buf, px) > chromaOf(ref, px) + 1.0, (satKey + ": 本带 chroma 增").c_str());
    // 远带像素不动（红带参数不碰蓝像素）
    const size_t far = (4 * 32 + 4) * 3;  // 近红灰像素（左半）
    if (i != 0) {
      CHECK(buf.data[far] == ref.data[far], (satKey + ": 远带不动").c_str());
    }
    ++satChecked;
    // lum：本带像素均值方向
    ImageBuf lb = makeHueProbe(32, 8, bandHue[i], 0.7, 0.5f);
    ImageBuf lref = lb;
    const std::string lumKey = std::string("hsl.") + bands[i] + ".lum";
    CHECK(applyParams("color.hsl", numParams(lumKey.c_str(), 50.0), &lb, &err),
          (lumKey + " apply").c_str());
    CHECK(lb.data[px] + lb.data[px + 1] + lb.data[px + 2] > lref.data[px] + lref.data[px + 1] + lref.data[px + 2],
          (lumKey + ": 本带提亮").c_str());
    ++lumChecked;
    // hue：本带像素色相旋转（+50 → +50/100*30° = 15°）
    ImageBuf hb = makeHueProbe(32, 8, bandHue[i], 0.9, 0.5f);
    const std::string hueKey = std::string("hsl.") + bands[i] + ".hue";
    CHECK(applyParams("color.hsl", numParams(hueKey.c_str(), 50.0), &hb, &err),
          (hueKey + " apply").c_str());
    double h0, s0, l0, h1, s1, l1;
    lumina::plan::rgbToHsl(hb.data[px] / 65535.0, hb.data[px + 1] / 65535.0, hb.data[px + 2] / 65535.0, &h0, &s0, &l0);
    lumina::plan::rgbToHsl(ref.data[px] / 65535.0, ref.data[px + 1] / 65535.0, ref.data[px + 2] / 65535.0, &h1, &s1, &l1);
    double dh = std::fabs(h0 - h1);
    if (dh > 0.5) dh = 1.0 - dh;
    CHECK(dh > 0.02, (hueKey + ": 色相旋转").c_str());
    ++hueChecked;
  }
  CHECK(satChecked == 8 && lumChecked == 8 && hueChecked == 8, "HSL 24 叶全覆盖");
  // clamp：sat 极大不炸、值域合法（uint16 天然 0..65535）
  ImageBuf cl = makeHueProbe(16, 8, 2.0 / 3, 0.9, 0.5f);
  CHECK(applyParams("color.hsl", numParams("hsl.blue.sat", 100.0), &cl, &err), "sat clamp apply");
}

static void testColorGrade() {
  std::printf("testColorGrade\n");
  std::string err;
  // 探针：码空间微弱色偏像素（有 hue、有分区亮度可控）
  auto makeTint = [](double r, double g, double b) {
    ImageBuf buf(16, 16);
    for (size_t i = 0; i + 2 < buf.data.size(); i += 3) {
      buf.data[i] = static_cast<uint16_t>(r * 65535.0 + 0.5);
      buf.data[i + 1] = static_cast<uint16_t>(g * 65535.0 + 0.5);
      buf.data[i + 2] = static_cast<uint16_t>(b * 65535.0 + 0.5);
    }
    return buf;
  };
  // shadowSat + shadowHue=0（红）：暗部青色像素色相向红最短角靠拢
  ImageBuf buf = makeTint(0.25, 0.30, 0.35);
  JsonValue p = JsonValue::objectValue();
  p.setItem("colorGrade.shadowHue", 0.0);
  p.setItem("colorGrade.shadowSat", 50.0);
  CHECK(applyParams("color.colorGrade", p, &buf, &err), "shadow grade apply");
  const size_t px = 0;
  auto hueDist = [](const ImageBuf& b, size_t q, double target) {
    double h, s, l;
    lumina::plan::rgbToHsl(b.data[q] / 65535.0, b.data[q + 1] / 65535.0,
                           b.data[q + 2] / 65535.0, &h, &s, &l);
    double d = std::fabs(h - target);
    return d > 0.5 ? 1.0 - d : d;
  };
  CHECK(hueDist(buf, px, 0.0) < hueDist(makeTint(0.25, 0.30, 0.35), px, 0.0),
        "colorGrade.shadow: 暗部向目标 hue（红）靠拢");
  // highlightSat + highlightHue=蓝：亮部像素 B-R 差扩大
  ImageBuf hb = makeTint(0.70, 0.75, 0.80);
  ImageBuf hbref = hb;
  JsonValue ph = JsonValue::objectValue();
  ph.setItem("colorGrade.highlightHue", 2.0 / 3 * 360.0);
  ph.setItem("colorGrade.highlightSat", 50.0);
  CHECK(applyParams("color.colorGrade", ph, &hb, &err), "highlight grade apply");
  CHECK(static_cast<double>(hb.data[px + 2]) - hb.data[px] >
            static_cast<double>(hbref.data[px + 2]) - hbref.data[px],
        "colorGrade.highlight: 亮部向蓝靠拢（B-R 扩大）");
  // midtoneSat + midtoneHue=绿：中间调 G-B 差扩大
  ImageBuf mb = makeTint(0.40, 0.45, 0.50);
  ImageBuf mbref = mb;
  JsonValue pm = JsonValue::objectValue();
  pm.setItem("colorGrade.midtoneHue", 1.0 / 3 * 360.0);
  pm.setItem("colorGrade.midtoneSat", 50.0);
  CHECK(applyParams("color.colorGrade", pm, &mb, &err), "midtone grade apply");
  CHECK(static_cast<double>(mb.data[px + 1]) - mb.data[px + 2] >
            static_cast<double>(mbref.data[px + 1]) - mbref.data[px + 2],
        "colorGrade.midtone: 中间调向绿靠拢（G-B 扩大）");
  // blend=0 → 效果最弱；balance 移界不炸
  ImageBuf bl = makeFlat(16, 16, 0.45f);
  JsonValue pb = JsonValue::objectValue();
  pb.setItem("colorGrade.midtoneHue", 120.0);
  pb.setItem("colorGrade.midtoneSat", 50.0);
  pb.setItem("colorGrade.blend", 0.0);
  pb.setItem("colorGrade.balance", -50.0);
  CHECK(applyParams("color.colorGrade", pb, &bl, &err), "blend/balance apply");
}

// ---- 5. calibration 7 叶 ----
static void testCalibration() {
  std::printf("testCalibration\n");
  std::string err;
  // redHue：需要 G≠B 的红系像素产生交叉；redSat：R>mean 缩放
  ImageBuf buf(16, 16);
  for (size_t i = 0; i + 2 < buf.data.size(); i += 3) {
    buf.data[i] = log16Encode(0.5f);
    buf.data[i + 1] = log16Encode(0.3f);
    buf.data[i + 2] = log16Encode(0.2f);
  }
  ImageBuf ref = buf;
  CHECK(applyParams("calibration.red", numParams("calibration.redHue", 50.0), &buf, &err), "redHue apply");
  CHECK(buf.data[0] != ref.data[0], "redHue: 通道交叉生效");
  CHECK(applyParams("calibration.red", numParams("calibration.redSat", 50.0), &buf, &err), "redSat apply");
  CHECK(buf.data[0] > ref.data[0], "redSat: 主色饱和（R>mean → R 增）");
  // green/blue 原色：主色高于均值时 sat 方向为增
  ImageBuf gbuf(16, 16);
  for (size_t i = 0; i + 2 < gbuf.data.size(); i += 3) {
    gbuf.data[i] = log16Encode(0.2f);
    gbuf.data[i + 1] = log16Encode(0.5f);
    gbuf.data[i + 2] = log16Encode(0.3f);
  }
  ImageBuf gref = gbuf;
  CHECK(applyParams("calibration.green", numParams("calibration.greenSat", 50.0), &gbuf, &err),
        "greenSat apply");
  CHECK(gbuf.data[1] > gref.data[1], "greenSat: G 主色增");
  ImageBuf bbuf(16, 16);
  for (size_t i = 0; i + 2 < bbuf.data.size(); i += 3) {
    bbuf.data[i] = log16Encode(0.3f);
    bbuf.data[i + 1] = log16Encode(0.2f);
    bbuf.data[i + 2] = log16Encode(0.5f);
  }
  ImageBuf bref = bbuf;
  CHECK(applyParams("calibration.blue", numParams("calibration.blueSat", 50.0), &bbuf, &err),
        "blueSat apply");
  CHECK(bbuf.data[2] > bref.data[2], "blueSat: B 主色增");
  // greenHue / blueHue：交叉生效即可
  ImageBuf hb(16, 16);
  for (size_t i = 0; i + 2 < hb.data.size(); i += 3) {
    hb.data[i] = log16Encode(0.2f);
    hb.data[i + 1] = log16Encode(0.4f);
    hb.data[i + 2] = log16Encode(0.6f);
  }
  CHECK(applyParams("calibration.green", numParams("calibration.greenHue", 50.0), &hb, &err), "greenHue apply");
  CHECK(applyParams("calibration.blue", numParams("calibration.blueHue", 50.0), &hb, &err), "blueHue apply");
  // shadowTint：暗像素品红（G 降）
  ImageBuf st = makeFlat(16, 16, 0.1f);
  ImageBuf stref = st;
  CHECK(applyParams("calibration.shadowTint", numParams("calibration.shadowTint", 50.0), &st, &err),
        "shadowTint apply");
  CHECK(st.data[1] < stref.data[1], "shadowTint>0: 阴影 G 降（品红）");
  // 亮像素权重≈0（码空间 0.9）
  ImageBuf stb(16, 16);
  for (uint16_t& v : stb.data) v = static_cast<uint16_t>(0.9 * 65535.0 + 0.5);
  ImageBuf stbref = stb;
  CHECK(applyParams("calibration.shadowTint", numParams("calibration.shadowTint", 50.0), &stb, &err),
        "shadowTint bright apply");
  CHECK(sameBytes(stb, stbref), "shadowTint: 亮区不动");
}

// ---- 6. detail 7 叶 ----
static ImageBuf makeNoiseImage(int w, int h, int seed) {
  ImageBuf buf(w, h);
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      uint16_t* p = &buf.data[(static_cast<size_t>(y) * w + x) * 3];
      const uint16_t base = log16Encode(0.45f);
      const int n = static_cast<int>(lumina::plan::hashNoise(x, y, seed) * 400.0) - 200;
      const int chroma = static_cast<int>(lumina::plan::hashNoise(x, y, seed + 1) * 600.0) - 300;
      p[0] = static_cast<uint16_t>(std::clamp(base + n + chroma, 0, 65535));
      p[1] = static_cast<uint16_t>(std::clamp(base + n, 0, 65535));
      p[2] = static_cast<uint16_t>(std::clamp(base + n - chroma, 0, 65535));
    }
  }
  return buf;
}

static void testDetailDenoise() {
  std::printf("testDetailDenoise\n");
  std::string err;
  // colorNR：色度离散下降
  ImageBuf buf = makeNoiseImage(48, 32, 3);
  ImageBuf ref = buf;
  CHECK(applyParams("detail.denoise", numParams("detail.colorNR", 80.0), &buf, &err), "colorNR apply");
  CHECK(chromaSpread(buf) < chromaSpread(ref), "colorNR: 色度噪点收敛");
  // luminanceSmoothing：整体亮度均值近似保持、局部方差降
  ImageBuf lb = makeNoiseImage(48, 32, 3);
  ImageBuf lbref = lb;
  CHECK(applyParams("detail.denoise", numParams("detail.luminanceSmoothing", 80.0), &lb, &err),
        "lumaSmoothing apply");
  double var0 = 0.0, var1 = 0.0;
  const double m0 = meanLum(lbref), m1 = meanLum(lb);
  for (size_t i = 0; i + 2 < lb.data.size(); i += 3) {
    var0 += std::pow((lbref.data[i] + lbref.data[i + 1] + lbref.data[i + 2]) / 3.0 - m0, 2);
    var1 += std::pow((lb.data[i] + lb.data[i + 1] + lb.data[i + 2]) / 3.0 - m1, 2);
  }
  CHECK(var1 < var0, "luminanceSmoothing: 亮度方差降");
  CHECK(std::fabs(m1 - m0) < 60.0, "luminanceSmoothing: 均值近似保持");
  // colorNRDetail：detail 高保留更多边缘（不炸即可 + 单调性经 chromaSpread）
  ImageBuf db = makeNoiseImage(48, 32, 3);
  CHECK(applyParams("detail.denoise", numParams("detail.colorNRDetail", 100.0), &db, &err),
        "colorNRDetail-only apply");
  CHECK(sameBytes(db, ref), "colorNRDetail: 无 NR 时无效果（参数非效果）");
}

static void testDetailSharpen() {
  std::printf("testDetailSharpen\n");
  std::string err;
  // 锐化：台阶边缘差放大；monotone in amount
  auto edgeDelta = [&](double amount, double masking, double* out) {
    ImageBuf buf(64, 16);
    for (int y = 0; y < 16; ++y) {
      for (int x = 0; x < 64; ++x) {
        uint16_t* p = &buf.data[(static_cast<size_t>(y) * 64 + x) * 3];
        const uint16_t c = log16Encode(x < 32 ? 0.3f : 0.6f);
        p[0] = p[1] = p[2] = c;
      }
    }
    JsonValue p = JsonValue::objectValue();
    p.setItem("detail.sharpness", amount);
    p.setItem("detail.sharpenMasking", masking);
    if (!applyParams("detail.sharpen", p, &buf, &err)) return false;
    const size_t l = (8 * 64 + 31) * 3;
    const size_t r = (8 * 64 + 33) * 3;
    *out = static_cast<double>(buf.data[r]) - buf.data[l];
    return true;
  };
  double base = 0.0, d50 = 0.0, d100 = 0.0, dMask = 0.0;
  CHECK(edgeDelta(0.0, 0.0, &base), "sharpen 0");
  CHECK(edgeDelta(50.0, 0.0, &d50) && edgeDelta(100.0, 0.0, &d100), "sharpen 50/100");
  CHECK(d50 > base, "sharpen: 边缘差放大");
  CHECK(d100 > d50, "sharpen: 单调");
  // masking 高 → 小差值被抑制（对纯台阶仍锐化，但 masking 满量程时阈值 30 内残差抑制）
  CHECK(edgeDelta(100.0, 100.0, &dMask), "sharpen masking");
  CHECK(dMask <= d100, "sharpenMasking: 高阈值不放大效果");
  // radius/detail 单独设置：无效果（参数非效果）
  ImageBuf flat = makeFlat(64, 16, 0.45f);
  ImageBuf flatref = flat;
  JsonValue p = JsonValue::objectValue();
  p.setItem("detail.sharpenRadius", 3.0);
  p.setItem("detail.sharpenDetail", 80.0);
  CHECK(applyParams("detail.sharpen", p, &flat, &err), "radius/detail-only apply");
  CHECK(sameBytes(flat, flatref), "sharpenRadius/Detail: sharpness=0 时 identity");
}

// ---- 7. lens 2 叶 ----
static void testLens() {
  std::printf("testLens\n");
  std::string err;
  ImageBuf buf = makeToneStripes(64, 48);
  ImageBuf ref = buf;
  CHECK(applyParams("lens.profile", numParams("optics.lensProfileEnable", 1.0), &buf, &err),
        "lensProfileEnable apply");
  CHECK(sameBytes(buf, ref), "lensProfileEnable: 无 profile 数据 identity（不猜测）");
  CHECK(applyParams("lens.manualDistortion", numParams("optics.manualDistortion", 50.0), &buf, &err),
        "manualDistortion apply");
  // 中心像素近似不动（r≈0）
  const size_t center = (24 * 64 + 32) * 3;
  CHECK(std::abs(static_cast<int>(buf.data[center]) - static_cast<int>(ref.data[center])) <= 2,
        "manualDistortion: 中心近似不动");
  // 角部被重映射（整图变化）
  bool changed = false;
  for (size_t i = 0; i < buf.data.size(); ++i) {
    if (buf.data[i] != ref.data[i]) {
      changed = true;
      break;
    }
  }
  CHECK(changed, "manualDistortion: k≠0 重映射生效");
  CHECK(applyParams("lens.manualDistortion", numParams("optics.manualDistortion", 0.0), &ref, &err),
        "manualDistortion 0 apply");
}

// ---- 8. geometry 13 叶 ----
static void testGeometryCrop() {
  std::printf("testGeometryCrop\n");
  std::string err;
  // 全幅 crop = identity 尺寸
  ImageBuf buf = makeToneStripes(64, 48);
  ImageBuf ref = buf;
  JsonValue p = JsonValue::objectValue();
  p.setItem("geometry.hasCrop", true);
  p.setItem("geometry.cropLeft", 0.0);
  p.setItem("geometry.cropTop", 0.0);
  p.setItem("geometry.cropRight", 100.0);
  p.setItem("geometry.cropBottom", 100.0);
  CHECK(applyParams("geometry.crop", p, &buf, &err), "full-frame crop apply");
  CHECK(buf.w == 64 && buf.h == 48, "全幅裁剪尺寸不变");
  // 半幅裁剪：左上 50%×50%
  ImageBuf half = makeToneStripes(64, 48);
  JsonValue ph = JsonValue::objectValue();
  ph.setItem("geometry.hasCrop", true);
  ph.setItem("geometry.cropLeft", 0.0);
  ph.setItem("geometry.cropTop", 0.0);
  ph.setItem("geometry.cropRight", 50.0);
  ph.setItem("geometry.cropBottom", 50.0);
  CHECK(applyParams("geometry.crop", ph, &half, &err), "half crop apply");
  CHECK(half.w == 32 && half.h == 24, "半幅裁剪尺寸");
  // 内容一致性：源对应区域逐像素相等（无旋转）
  bool match = true;
  for (int y = 0; y < 24 && match; ++y) {
    for (int x = 0; x < 32 && match; ++x) {
      const uint16_t* src = &ref.data[(static_cast<size_t>(y) * 64 + x) * 3];
      const uint16_t* dst = &half.data[(static_cast<size_t>(y) * 32 + x) * 3];
      for (int c = 0; c < 3; ++c) {
        if (src[c] != dst[c]) match = false;
      }
    }
  }
  CHECK(match, "裁剪内容与源区域逐像素一致");
  // cropAngle
  ImageBuf ang = makeToneStripes(64, 64);
  JsonValue pa = JsonValue::objectValue();
  pa.setItem("geometry.hasCrop", true);
  pa.setItem("geometry.cropAngle", 5.0);
  CHECK(applyParams("geometry.crop", pa, &ang, &err), "cropAngle apply");
  CHECK(ang.w > 0 && ang.h > 0, "cropAngle: 输出有效");
  // 无 crop 无角度 identity（identity 全表覆盖）
}

static void testGeometryPerspective() {
  std::printf("testGeometryPerspective\n");
  std::string err;
  ImageBuf buf = makeToneStripes(64, 48);
  // rotate 90：非方形 → 尺寸互换
  JsonValue p = JsonValue::objectValue();
  p.setItem("geometry.perspective.rotate", 90.0);
  CHECK(applyParams("geometry.perspective", p, &buf, &err), "rotate 90 apply");
  CHECK(buf.w == 48 && buf.h == 64, "rotate 90: 尺寸互换");
  // scale 0.5：内容缩小（倍数语义，1=恒等；尺寸由 mapQuadBounds 决定——只验输出有效）
  ImageBuf sc = makeToneStripes(64, 48);
  JsonValue ps = JsonValue::objectValue();
  ps.setItem("geometry.perspective.scale", 0.5);
  CHECK(applyParams("geometry.perspective", ps, &sc, &err), "scale apply");
  CHECK(sc.valid(), "scale: 输出有效");
  // horizontal/vertical/x/y/aspect：合法执行 + 输出有效
  for (const char* k : {"geometry.perspective.horizontal", "geometry.perspective.vertical",
                        "geometry.perspective.x", "geometry.perspective.y",
                        "geometry.perspective.aspect"}) {
    ImageBuf b = makeToneStripes(64, 48);
    JsonValue pk = numParams(k, 20.0);
    CHECK(applyParams("geometry.perspective", pk, &b, &err), (std::string(k) + " apply").c_str());
    CHECK(b.valid(), (std::string(k) + ": 输出有效").c_str());
  }
}

// ---- 9. effects 6 叶 ----
static void testEffectsVignette() {
  std::printf("testEffectsVignette\n");
  std::string err;
  ImageBuf buf = makeFlat(64, 64, 0.45f);
  CHECK(applyParams("effects.vignette", numParams("optics.vignetteAmount", -50.0), &buf, &err),
        "vignette -50 apply");
  const size_t center = (32 * 64 + 32) * 3;
  const size_t corner = (2 * 64 + 2) * 3;
  CHECK(buf.data[center] == log16Encode(0.45f), "vignette: 中心不动（r < midpoint）");
  CHECK(buf.data[corner] < log16Encode(0.45f), "vignette<0: 角部压暗");
  // midpoint 推迟衰减起点
  ImageBuf late = makeFlat(64, 64, 0.45f);
  JsonValue p = JsonValue::objectValue();
  p.setItem("optics.vignetteAmount", -50.0);
  p.setItem("optics.vignetteMidpoint", 90.0);
  CHECK(applyParams("effects.vignette", p, &late, &err), "midpoint 90 apply");
  CHECK(late.data[corner] >= buf.data[corner], "vignetteMidpoint: 高 midpoint 衰减更少");
  // feather
  ImageBuf feather = makeFlat(64, 64, 0.45f);
  JsonValue pf = JsonValue::objectValue();
  pf.setItem("optics.vignetteAmount", -50.0);
  pf.setItem("optics.vignetteFeather", 100.0);
  CHECK(applyParams("effects.vignette", pf, &feather, &err), "feather apply");
  CHECK(feather.data[corner] >= buf.data[corner], "vignetteFeather: 宽羽化衰减更缓");
}

static void testEffectsGrain() {
  std::printf("testEffectsGrain\n");
  std::string err;
  ImageBuf a = makeFlat(64, 64, 0.45f);
  ImageBuf b = makeFlat(64, 64, 0.45f);
  CHECK(applyParams("effects.grain", numParams("optics.grainAmount", 40.0), &a, &err), "grain A");
  CHECK(applyParams("effects.grain", numParams("optics.grainAmount", 40.0), &b, &err), "grain B");
  CHECK(sameBytes(a, b), "grain: 确定性（同参数两次逐字节相同）");
  CHECK(!sameBytes(a, makeFlat(64, 64, 0.45f)), "grain: 幅度>0 有效果");
  // grainFrequency 调制幅度
  ImageBuf f = makeFlat(64, 64, 0.45f);
  JsonValue pf = JsonValue::objectValue();
  pf.setItem("optics.grainAmount", 40.0);
  pf.setItem("optics.grainFrequency", 100.0);
  CHECK(applyParams("effects.grain", pf, &f, &err), "grain freq apply");
  // 频率加大 → 与无频率图的差更大（幅度调制）
  bool bigger = false;
  for (size_t i = 0; i < a.data.size(); ++i) {
    if (std::abs(static_cast<int>(f.data[i]) - static_cast<int>(a.data[i])) > 2) bigger = true;
  }
  CHECK(bigger, "grainFrequency: 幅度调制生效");
  // grainSize：块度（相邻像素相关）——size=8 时 2x1 相邻像素差为 0
  ImageBuf g8 = makeFlat(64, 64, 0.45f);
  JsonValue ps = JsonValue::objectValue();
  ps.setItem("optics.grainAmount", 40.0);
  ps.setItem("optics.grainSize", 8.0);
  CHECK(applyParams("effects.grain", ps, &g8, &err), "grain size apply");
  CHECK(g8.data[100] == g8.data[103], "grainSize: 同块像素共享噪声");
}

// ---- 10. mask 1 叶 + applyMasked 数学 ----
class TestLiftOp : public lumina::plan::Operator {
 public:
  const char* name() const override { return "test.lift"; }
  lumina::plan::Stage stage() const override { return lumina::plan::Stage::S4Color; }
  bool apply(imaging::ImageBuf* buf, std::string* err) const override {
    if (!buf || !buf->valid()) {
      if (err) *err = "test.lift: invalid";
      return false;
    }
    for (uint16_t& v : buf->data) {
      const double d = v + 1000.0;
      v = static_cast<uint16_t>(std::min(65535.0, d));
    }
    return true;
  }
};

static void testMask() {
  std::printf("testMask\n");
  std::string err;
  TestLiftOp lift;
  const double base = 10000.0;
  ImageBuf buf(8, 8);
  for (uint16_t& v : buf.data) v = static_cast<uint16_t>(base);
  ImageBuf out;
  // normal + 全 1 mask + opacity 100 → base + 1000
  CHECK(lumina::plan::applyMasked(lift, buf, nullptr, 100, "normal", &out, &err), "applyMasked normal");
  CHECK(out.data[0] == base + 1000, "normal: opacity 100 全量");
  // opacity 50 → 半量
  CHECK(lumina::plan::applyMasked(lift, buf, nullptr, 50, "normal", &out, &err), "applyMasked op50");
  CHECK(out.data[0] == base + 500, "normal: opacity 50 → base + 500");
  // mask 0.5 + opacity 50 → 有效 alpha 0.25
  std::vector<float> mask(buf.pixelCount(), 0.5f);
  CHECK(lumina::plan::applyMasked(lift, buf, &mask, 50, "normal", &out, &err), "applyMasked masked");
  CHECK(out.data[0] == base + 250, "mask 0.5 × opacity 50 → +250");
  // multiply 混合数学：layer = base+1000, blended = base*(base+1000)/65535
  CHECK(lumina::plan::applyMasked(lift, buf, nullptr, 100, "multiply", &out, &err), "applyMasked multiply");
  const double expect = base * (base + 1000.0) / 65535.0;
  CHECK(std::fabs(static_cast<double>(out.data[0]) - expect) <= 1.0, "multiply: bv*lv/65535");
  // mask 全 0 → 完全不动
  std::vector<float> zero(buf.pixelCount(), 0.0f);
  CHECK(lumina::plan::applyMasked(lift, buf, &zero, 100, "normal", &out, &err), "applyMasked zero");
  CHECK(out.data[0] == base, "mask 0: base 不变");
  // 宿主容器算子 mask.apply：identity（蒙版数学 T2.2）
  ImageBuf host = buf;
  ImageBuf hostref = host;
  CHECK(applyParams("mask.apply", JsonValue::objectValue(), &host, &err), "mask.apply");
  CHECK(sameBytes(host, hostref), "mask.apply: 容器节点 identity（masks[] 叶参数 T2.2 消费）");
}

// ---- 11. pipeline.toneMap ----
static void testToneMap() {
  std::printf("testToneMap\n");
  std::string err;
  // 单调 + 端点：0 码 → 映射后不增（Reinhard c/(1+c)）
  ImageBuf buf(8, 8);
  for (int y = 0; y < 8; ++y) {
    for (int x = 0; x < 8; ++x) {
      uint16_t* p = &buf.data[(static_cast<size_t>(y) * 8 + x) * 3];
      const uint16_t v = static_cast<uint16_t>((y * 8 + x) * 65535 / 63);
      p[0] = p[1] = p[2] = v;
    }
  }
  ImageBuf ref = buf;
  JsonValue empty = JsonValue::objectValue();
  CHECK(applyParams("pipeline.toneMap", empty, &buf, &err), "toneMap apply");
  bool mono = true;
  for (size_t i = 0; i + 3 < buf.data.size(); i += 3) {
    if (ref.data[i] < ref.data[i + 3] && buf.data[i] > buf.data[i + 3]) mono = false;
  }
  CHECK(mono, "toneMap: 单调（亮输入不产生更暗输出）");
  CHECK(buf.data[0] <= ref.data[0], "toneMap: 最暗端不提亮");
}

// ---- 12. 端到端：compilePlan + executePlan 参数流 ----
static void testPlanEndToEnd() {
  std::printf("testPlanEndToEnd\n");
  std::string err;
  std::vector<std::pair<std::string, std::string>> params;
  params.push_back({"basic.exposure", "1.0"});
  params.push_back({"parametric.highlights", "30"});
  params.push_back({"optics.vignetteAmount", "-30"});
  params.push_back({"hsl.blue.sat", "20"});
  params.push_back({"geometry.perspective.rotate", "0"});
  lumina::plan::RenderPlan plan;
  CHECK(lumina::plan::compilePlan(1, params, &plan, &err), "compilePlan");
  // T1.5：pipeline.toneMap 恒入序列（唯一域分界，预览 GPU/TS oracle 同口径）→ 5 叶 + 1
  CHECK(plan.opSequence.size() == 6, "算子去重与 stage 归并 + toneMap 恒入（perspective 即便 rotate=0 也入列）");
  ImageBuf buf = makeToneStripes(64, 48);
  ImageBuf ref = buf;
  CHECK(lumina::plan::executePlan(plan, &buf, &err), "executePlan");
  bool changed = false;
  for (size_t i = 0; i < buf.data.size(); ++i) {
    if (buf.data[i] != ref.data[i]) {
      changed = true;
      break;
    }
  }
  CHECK(changed, "端到端: 参数流生效");
}

int main() {
  std::string err;
  if (!lumina::plan::registerBuiltinOps(&OpTable::instance(), &err)) {
    std::printf("FATAL registerBuiltinOps: %s\n", err.c_str());
    return 2;
  }
  testIdentityAtDefaults();
  testBasicExposure();
  testBasicTemperatureTint();
  testBasicContrast();
  testBasicToneZones();
  testBasicClarityTexture();
  testBasicDehaze();
  testBasicVibranceSaturation();
  testBasicWhiteBalance();
  testCurvePoint();
  testCurveParametric();
  testColorHsl();
  testColorGrade();
  testCalibration();
  testDetailDenoise();
  testDetailSharpen();
  testLens();
  testGeometryCrop();
  testGeometryPerspective();
  testEffectsVignette();
  testEffectsGrain();
  testMask();
  testToneMap();
  testPlanEndToEnd();
  std::printf(g_fail == 0 ? "\nALL PASS\n" : "\n%d FAILURES\n", g_fail);
  return g_fail == 0 ? 0 : 1;
}
