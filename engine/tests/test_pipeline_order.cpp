// M2 T2.1 验收：20 阶段顺序表断言（O1）+ 顺序敏感反例（O2）
//
// O1（三组，架构 §3.7）：
//   ① kPipelineOrder 行序自检：行号 1..20 连续、stage 单调不减、每行 stage 与
//      架构 §3.7 表逐行相等、行内算子全部已注册；
//   ② 全参 Plan 编译后 opSequence 的 Stage 序列单调不减；
//   ③ opSequence 的每一行都能在 kPipelineOrder 里找到归属行，且行号单调不减
//      （= 与算子级行构成子序列，架构 §3.7 的「第 2–6 行是 geometry.apply 内部子步」）。
//
// O2（顺序敏感反例）：把两个算子换序，输出 max|Δ| 必须 ≥ 2 LSB（8bit）。
//   若换序后像素不变，说明「顺序」在实现里没被真实执行（K1「一致地错」同类风险）。
//
// 注意（实测发现，已上报架构 v1.1 §3.7 并按其要求补强）：
//   - O2 分两组判据：
//     · **语义性组**（不依赖 clamp）：curve 前置 toneMap（36.52 LSB）、exposure→highlights
//       （5.52 LSB，且夹具 clipped=0 证明未触及码域上限）、curve→sharpen（6.98 LSB）。
//       语义性组的成立原因是「算子非线性且耦合」——toneMap/曲线是 LUT，highlights 的
//       权重按像素亮度连续变化，换序必然改变结果。
//     · **码域性组**（依赖 clamp）：clarity↔exposure。两者都是**线性**算子
//       （clarity = u + k(u−blur u)，box 模糊线性；exposure = 码值平移），无 clamp 时
//       严格可交换。testClampDependency 用「同一对算子 + 同一参数、只改夹具亮度区间」
//       把这条依赖显式断言下来（无裁切 Δ=0.00，有裁切 Δ=3.78，clipped 6528）。
//   - 锐化↔降噪**不是**语义性反例：现有实现的降噪是盒式模糊、锐化是反遮罩，两者都线性，
//     实测换序 Δ = 0.46 LSB。测试把「不成立」显式断言，防止后人误加假反例。
//   - mask.apply 在 T2.6 之前仍是 IdentityOp，因此「蒙版前置于曲线」的 Δ 断言
//     暂记 SKIP（并打印 T2.6 待接续）；其**结构**断言（mask.apply 行号 > 曲线行号）
//     现在就成立。M2 出口要求 O2 零 SKIP（架构 v1.1），T2.6 必须消掉这条。
#include <algorithm>
#include <cmath>
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
using lumina::json::JsonValue;
using lumina::plan::OpTable;
using lumina::plan::PipelineStageDesc;
using lumina::plan::Stage;
using lumina::plan::stageIndex;

static int g_fail = 0;
static int g_skip = 0;
#define CHECK(cond, msg)                                                 \
  do {                                                                   \
    if (!(cond)) {                                                       \
      std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, msg);           \
      ++g_fail;                                                          \
    } else {                                                             \
      std::printf("  [PASS] %s\n", msg);                                 \
    }                                                                    \
  } while (0)

// O1 的门槛常量（S10：只许收紧）。8bit LSB = 256 个 log16 码值。
static constexpr double kOrderDeltaThresholdLsb = 2.0;

namespace {

ImageBuf makeFlat(int w, int h, double v) {
  ImageBuf buf(w, h);
  const uint16_t c = static_cast<uint16_t>(v * 65535.0 + 0.5);
  for (size_t i = 0; i < buf.data.size(); ++i) buf.data[i] = c;
  return buf;
}

// O2 夹具：亮度斜坡 0.10..1.00 + 细纹。斜坡顶端在 +EV 后进入码域 clamp，
// 使「clarity/contrast 与 exposure 换序」产生真实可见差异（详见文件头说明）。
ImageBuf makeOrderFixture(int w, int h) {
  ImageBuf buf(w, h);
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      const double t = (w > 1) ? static_cast<double>(x) / (w - 1) : 0.0;
      double v = 0.10 + 0.90 * t;
      v += ((x % 3) - 1) * 0.030;  // 细纹（clarity/锐化有残差可放大）
      v += ((y % 3) - 1) * 0.015;
      if (v < 0.0) v = 0.0;
      if (v > 1.0) v = 1.0;
      const uint16_t c = static_cast<uint16_t>(v * 65535.0 + 0.5);
      uint16_t* p = &buf.data[(static_cast<size_t>(y) * w + x) * 3];
      p[0] = c;
      p[1] = c;
      p[2] = c;
    }
  }
  return buf;
}

// 可指定亮度区间的夹具（用于证明「clarity↔exposure 的不可交换性来自码域 clamp」）
ImageBuf makeRampFixture(int w, int h, double lo, double hi) {
  ImageBuf buf(w, h);
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      const double t = (w > 1) ? static_cast<double>(x) / (w - 1) : 0.0;
      double v = lo + (hi - lo) * t;
      v += ((x % 3) - 1) * 0.030;  // 细纹（与 makeOrderFixture 同幅度：clamp 效应靠残差放大）
      if (v < 0.0) v = 0.0;
      if (v > 1.0) v = 1.0;
      const uint16_t c = static_cast<uint16_t>(v * 65535.0 + 0.5);
      uint16_t* p = &buf.data[(static_cast<size_t>(y) * w + x) * 3];
      p[0] = c;
      p[1] = c;
      p[2] = c;
    }
  }
  return buf;
}

// 处于码域上限（65535）的样本数：clamp 是否真的发生，用计数证明而不是靠嘴说
size_t countClipped(const ImageBuf& b) {
  size_t n = 0;
  for (uint16_t v : b.data) {
    if (v == 65535) ++n;
  }
  return n;
}

// 带确定性色噪的夹具：色度降噪（detail.denoise/colorNR）必须有可降的色噪，
// 否则 denoise 是 no-op，任何「顺序」都不会产生差异（假绿）。
ImageBuf makeNoisyFixture(int w, int h) {
  ImageBuf buf(w, h);
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      const double t = (w > 1) ? static_cast<double>(x) / (w - 1) : 0.0;
      const double base = 0.25 + 0.35 * t;
      uint16_t* p = &buf.data[(static_cast<size_t>(y) * w + x) * 3];
      for (int c = 0; c < 3; ++c) {
        double r = std::sin(x * 12.9898 + y * 78.233 + c * 37.719) * 43758.5453;
        r -= std::floor(r);           // [0,1)
        double v = base + (r * 2.0 - 1.0) * 0.05;  // 每通道独立 → 真色噪
        if (v < 0.0) v = 0.0;
        if (v > 1.0) v = 1.0;
        p[c] = static_cast<uint16_t>(v * 65535.0 + 0.5);
      }
    }
  }
  return buf;
}

JsonValue numParams(const char* key, double v) {
  JsonValue p = JsonValue::objectValue();
  p.setItem(key, v);
  return p;
}

// 点曲线参数：{x,y}[]，x/y 为 0..255
JsonValue curveParams(const char* key) {
  JsonValue pts = JsonValue::arrayValue();
  const double xy[3][2] = {{0.0, 0.0}, {128.0, 176.0}, {255.0, 255.0}};
  for (const auto& p : xy) {
    JsonValue o = JsonValue::objectValue();
    o.setItem("x", p[0]);
    o.setItem("y", p[1]);
    pts.addItem(o);
  }
  JsonValue params = JsonValue::objectValue();
  params.setItem(key, pts);
  return params;
}

// 单算子施加（每次重新注入参数；OpTable 的 setParams 有状态）
bool runOne(const std::string& op, const JsonValue& params, ImageBuf* buf, std::string* err) {
  std::vector<std::string> names{op};
  std::map<std::string, JsonValue> byOp{{op, params}};
  return OpTable::instance().evaluate(names, buf, byOp, err);
}

struct Step {
  std::string op;
  JsonValue params;
};

bool runSeq(const std::vector<Step>& seq, ImageBuf* buf, std::string* err) {
  for (const Step& s : seq) {
    if (!runOne(s.op, s.params, buf, err)) return false;
  }
  return true;
}

// max|Δ|（8bit LSB 尺度）
double maxAbsDeltaLsb(const ImageBuf& a, const ImageBuf& b) {
  if (a.w != b.w || a.h != b.h || a.data.size() != b.data.size()) return 1e9;
  double m = 0.0;
  for (size_t i = 0; i < a.data.size(); ++i) {
    const double d = std::fabs(static_cast<double>(a.data[i]) - static_cast<double>(b.data[i])) /
                     256.0;
    if (d > m) m = d;
  }
  return m;
}

// 编译全参 Plan（93 叶口径：92 个标量叶 + masks[]；icc 属 M4，不在此列）
std::vector<std::pair<std::string, JsonValue>> fullParams() {
  static const char* kLeaves[] = {
      "basic.whiteBalance", "basic.temperature", "basic.tint", "basic.exposure",
      "basic.contrast", "basic.highlights", "basic.shadows", "basic.whites",
      "basic.blacks", "basic.clarity", "basic.texture", "basic.dehaze",
      "basic.vibrance", "basic.saturation",
      "curve.toneCurveRGB", "curve.toneCurveR", "curve.toneCurveG", "curve.toneCurveB",
      "parametric.highlights", "parametric.lights", "parametric.darks", "parametric.shadows",
      "parametric.shadowSplit", "parametric.lightSplit", "parametric.midSplit",
      "hsl.red.hue", "hsl.red.sat", "hsl.red.lum", "hsl.orange.hue", "hsl.orange.sat",
      "hsl.orange.lum", "hsl.yellow.hue", "hsl.yellow.sat", "hsl.yellow.lum", "hsl.green.hue",
      "hsl.green.sat", "hsl.green.lum", "hsl.aqua.hue", "hsl.aqua.sat", "hsl.aqua.lum",
      "hsl.blue.hue", "hsl.blue.sat", "hsl.blue.lum", "hsl.purple.hue", "hsl.purple.sat",
      "hsl.purple.lum", "hsl.magenta.hue", "hsl.magenta.sat", "hsl.magenta.lum",
      "colorGrade.shadowHue", "colorGrade.shadowSat", "colorGrade.midtoneHue",
      "colorGrade.midtoneSat", "colorGrade.highlightHue", "colorGrade.highlightSat",
      "colorGrade.blend", "colorGrade.balance",
      "calibration.shadowTint", "calibration.redHue", "calibration.redSat",
      "calibration.greenHue", "calibration.greenSat", "calibration.blueHue",
      "calibration.blueSat",
      "detail.sharpness", "detail.sharpenRadius", "detail.sharpenDetail", "detail.sharpenMasking",
      "detail.luminanceSmoothing", "detail.colorNR", "detail.colorNRDetail",
      "optics.lensProfileEnable", "optics.manualDistortion", "optics.vignetteAmount",
      "optics.vignetteMidpoint", "optics.vignetteFeather", "optics.grainAmount",
      "optics.grainFrequency", "optics.grainSize",
      "geometry.hasCrop", "geometry.cropTop", "geometry.cropBottom", "geometry.cropLeft",
      "geometry.cropRight", "geometry.cropAngle",
      "geometry.perspective.vertical", "geometry.perspective.horizontal",
      "geometry.perspective.rotate", "geometry.perspective.scale", "geometry.perspective.aspect",
      "geometry.perspective.x", "geometry.perspective.y",
  };
  std::vector<std::pair<std::string, JsonValue>> out;
  for (const char* leaf : kLeaves) out.push_back({std::string(leaf), JsonValue::number(0.0)});
  // masks 是数组整叶（S1）；空数组即可让 mask.apply 进入序列
  out.push_back({"masks", JsonValue::arrayValue()});
  return out;
}

const char* stageLabel(Stage s) {
  switch (s) {
    case Stage::S0Decode: return "S0Decode";
    case Stage::S1Geometry: return "S1Geometry";
    case Stage::S15Denoise: return "S15Denoise";
    case Stage::S2Tone: return "S2Tone";
    case Stage::S3ToneMap: return "S3ToneMap";
    case Stage::S4Color: return "S4Color";
    case Stage::S5Detail: return "S5Detail";
    case Stage::S6Effects: return "S6Effects";
    case Stage::S7Output: return "S7Output";
    default: return "?";
  }
}

}  // namespace

// ---------------- O1 ① ----------------
static void testPipelineOrderTable() {
  std::printf("O1-① kPipelineOrder 行序自检\n");
  static_assert(lumina::plan::kPipelineOrderCount == 20, "架构 §3.7：20 阶段顺序表");
  CHECK(lumina::plan::kPipelineOrderCount == 20, "kPipelineOrderCount == 20");
  // 架构 §3.7 逐行的 stage 期望（顺序被改动即失败）
  const Stage kExpected[20] = {
      Stage::S0Decode,
      Stage::S1Geometry, Stage::S1Geometry, Stage::S1Geometry, Stage::S1Geometry,
      Stage::S1Geometry,
      Stage::S15Denoise,
      Stage::S2Tone, Stage::S2Tone, Stage::S2Tone, Stage::S2Tone, Stage::S2Tone, Stage::S2Tone,
      Stage::S2Tone,
      Stage::S3ToneMap,
      Stage::S4Color, Stage::S4Color, Stage::S4Color,
      Stage::S5Detail,
      Stage::S6Effects,
  };
  bool indexOk = true, stageOk = true, monotonic = true, opsOk = true, registered = true;
  bool subStepOk = true;
  int lastSubStep = 0;
  for (int i = 0; i < lumina::plan::kPipelineOrderCount; ++i) {
    const PipelineStageDesc& r = lumina::plan::kPipelineOrder[i];
    if (r.index != i + 1) indexOk = false;
    if (r.stage != kExpected[i]) stageOk = false;
    if (i > 0 && stageIndex(r.stage) < stageIndex(lumina::plan::kPipelineOrder[i - 1].stageEnd)) {
      monotonic = false;
    }
    if (r.subStep > 0) {
      if (r.subStep <= lastSubStep) subStepOk = false;
      lastSubStep = r.subStep;
    } else if (r.ops == nullptr || std::string(r.ops).empty()) {
      opsOk = false;  // 独立算子行必须登记算子名
    }
    // 行内算子必须真的注册过（防止表里写错名字导致断言空转）
    const std::string ops = r.ops ? r.ops : "";
    size_t start = 0;
    while (!ops.empty() && start <= ops.size()) {
      size_t comma = ops.find(',', start);
      if (comma == std::string::npos) comma = ops.size();
      const std::string one = ops.substr(start, comma - start);
      if (!one.empty()) {
        if (!OpTable::instance().find(one)) {
          std::printf("    [缺失算子] 行 %d 登记了未注册算子: %s\n", r.index, one.c_str());
          registered = false;
        }
        if (lumina::plan::pipelineRowForOp(one) != r.index) {
          std::printf("    [行号不一致] %s → %d（表里是 %d）\n", one.c_str(),
                      lumina::plan::pipelineRowForOp(one), r.index);
          registered = false;
        }
      }
      if (comma == ops.size()) break;
      start = comma + 1;
    }
  }
  CHECK(indexOk, "行号 1..20 连续无跳号");
  CHECK(stageOk, "每行 stage 与架构 §3.7 表逐行相等");
  CHECK(monotonic, "stage 序列单调不减（含第 20 行 S6Effects→S7Output）");
  CHECK(subStepOk, "geometry.apply 子步行（2–6）的 subStep 严格递增");
  CHECK(opsOk, "每个独立算子行都登记了算子名");
  CHECK(registered, "表内算子全部已注册且 pipelineRowForOp 行号自洽");
  // 第 20 行跨 stage 的两端必须都在
  CHECK(lumina::plan::kPipelineOrder[19].stage == Stage::S6Effects &&
            lumina::plan::kPipelineOrder[19].stageEnd == Stage::S7Output,
        "第 20 行跨 S6Effects → S7Output（暗角/颗粒 + 输出编码）");
}

// ---------------- O1 ② + ③ ----------------
static void testPlanSequenceOrder() {
  std::printf("O1-②③ 全参 Plan 的 stage 单调 + 算子级子序列\n");
  const auto params = fullParams();
  lumina::plan::RenderPlan plan;
  std::string err;
  CHECK(lumina::plan::compilePlan(1, params, &plan, &err), "compilePlan 全参成功");
  if (g_fail) return;
  std::printf("    opSequence(%zu):", plan.opSequence.size());
  for (const std::string& s : plan.opSequence) std::printf(" %s", s.c_str());
  std::printf("\n");

  // ② stage 单调不减
  bool monotonic = true;
  int last = -1;
  std::string lastOp;
  for (const std::string& name : plan.opSequence) {
    const lumina::plan::Operator* op = OpTable::instance().find(name);
    if (!op) {
      CHECK(false, (std::string("opSequence 含未注册算子: ") + name).c_str());
      return;
    }
    const int si = stageIndex(op->stage());
    if (si < last) {
      std::printf("    [逆序] %s(%s) 出现在 %s(%s) 之后\n", name.c_str(), stageLabel(op->stage()),
                  lastOp.c_str(), stageLabel(static_cast<Stage>(last)));
      monotonic = false;
    }
    last = si;
    lastOp = name;
  }
  CHECK(monotonic, "opSequence 的 Stage 序列单调不减");

  // ③ 行号单调不减（= 与 kPipelineOrder 的算子级行构成子序列）
  bool rowOk = true;
  int lastRow = 0;
  std::string lastRowOp;
  for (const std::string& name : plan.opSequence) {
    const int row = lumina::plan::pipelineRowForOp(name);
    if (row == 0) {
      std::printf("    [未登记] opSequence 的算子 %s 不在 20 阶段表内\n", name.c_str());
      rowOk = false;
      continue;
    }
    if (row < lastRow) {
      std::printf("    [行号回退] %s → 行 %d，前一个 %s 已在行 %d\n", name.c_str(), row,
                  lastRowOp.c_str(), lastRow);
      rowOk = false;
    }
    lastRow = row;
    lastRowOp = name;
  }
  CHECK(rowOk, "opSequence 全部算子可归属 20 阶段表且行号单调不减");
  // toneMap 必须恒在序列内（S3 唯一域分界）
  CHECK(std::find(plan.opSequence.begin(), plan.opSequence.end(), "pipeline.toneMap") !=
            plan.opSequence.end(),
        "pipeline.toneMap 恒入序列（S3 域分界不可省）");
  // 蒙版必须排在所有曲线之后（结构断言，O2-c 的一部分）
  const int maskRow = lumina::plan::pipelineRowForOp("mask.apply");
  const int curveRow = lumina::plan::pipelineRowForOp("curve.toneCurveRGB");
  CHECK(maskRow > curveRow, "mask.apply 的行号 > 曲线行号（蒙版在曲线之后，架构 §1.2）");
}

// ---------------- O2：顺序敏感反例 ----------------
// 通用：给定「正确顺序」与「错误顺序」，断言两者输出差 ≥ kOrderDeltaThresholdLsb
static void checkOrderSensitive(const char* name, const std::vector<Step>& correct,
                                const std::vector<Step>& wrong,
                                const ImageBuf* baseOverride = nullptr) {
  const ImageBuf base = baseOverride ? *baseOverride : makeOrderFixture(192, 128);
  ImageBuf a = base;
  ImageBuf b = base;
  std::string err;
  if (!runSeq(correct, &a, &err)) {
    CHECK(false, (std::string(name) + " 正确顺序执行失败: " + err).c_str());
    return;
  }
  if (!runSeq(wrong, &b, &err)) {
    CHECK(false, (std::string(name) + " 错误顺序执行失败: " + err).c_str());
    return;
  }
  const double d = maxAbsDeltaLsb(a, b);
  std::printf("    %-34s max|Δ| = %.2f LSB（门槛 %.1f）\n", name, d, kOrderDeltaThresholdLsb);
  CHECK(d >= kOrderDeltaThresholdLsb,
        (std::string("顺序敏感: ") + name + " max|Δ| ≥ 2 LSB").c_str());
}

static void testOrderSensitiveCounterexamples() {
  std::printf("O2 顺序敏感反例（三组指定 + 两组补强）\n");
  // 组 1：clarity ↔ exposure 互换（架构 §3.7 指定）
  // 无裁切时二者严格可交换，差异只来自码域 clamp → 用 +2.5EV 让高光进入裁切
  {
    const JsonValue clarity = numParams("basic.clarity", 80.0);
    const JsonValue exposure = numParams("basic.exposure", 2.5);
    checkOrderSensitive("clarity→exposure（正确）vs 互换",
                        {Step{"basic.clarity", clarity}, Step{"basic.exposure", exposure}},
                        {Step{"basic.exposure", exposure}, Step{"basic.clarity", clarity}});
  }
  // 组 1b：contrast ↔ exposure（仿射 vs 平移，天然不可交换——无裁切也成立）
  {
    const JsonValue contrast = numParams("basic.contrast", 60.0);
    const JsonValue exposure = numParams("basic.exposure", 0.8);
    checkOrderSensitive("exposure→contrast（正确）vs 互换",
                        {Step{"basic.exposure", exposure}, Step{"basic.contrast", contrast}},
                        {Step{"basic.contrast", contrast}, Step{"basic.exposure", exposure}});
  }
  // 组 2：曲线前置于 toneMap（架构 §3.7 指定）
  {
    const JsonValue curve = curveParams("curve.toneCurveRGB");
    const JsonValue tone = JsonValue::objectValue();
    checkOrderSensitive("toneMap→curve（正确）vs 曲线前置",
                        {Step{"pipeline.toneMap", tone}, Step{"curve.toneCurveRGB", curve}},
                        {Step{"curve.toneCurveRGB", curve}, Step{"pipeline.toneMap", tone}});
  }
  // 组 3：蒙版前置于曲线（架构 §3.7 指定）。
  // mask.apply 在 T2.6 之前是 IdentityOp → 先用「非平凡参数施加后像素是否改变」探测；
  // 未实装则记 SKIP 并显式打印待接续任务（零静默：不假装通过）。
  {
    const JsonValue curve = curveParams("curve.toneCurveRGB");
    JsonValue maskParams = JsonValue::objectValue();
    JsonValue masks = JsonValue::arrayValue();
    JsonValue m = JsonValue::objectValue();
    m.setItem("id", "m1");
    m.setItem("type", "radial");
    m.setItem("enabled", true);
    m.setItem("opacity", 100);
    m.setItem("amount", 100);
    JsonValue geo = JsonValue::objectValue();
    geo.setItem("cx", 0.5);
    geo.setItem("cy", 0.5);
    geo.setItem("radius", 0.4);
    m.setItem("geometry", geo);
    JsonValue local = JsonValue::objectValue();
    local.setItem("exposure", 1.0);
    m.setItem("local", local);
    masks.addItem(m);
    maskParams.setItem("masks", masks);

    ImageBuf probe = makeOrderFixture(192, 128);
    const ImageBuf probeRef = probe;
    std::string err;
    const bool probeOk = runOne("mask.apply", maskParams, &probe, &err);
    const double probeDelta = probeOk ? maxAbsDeltaLsb(probeRef, probe) : 0.0;
    if (probeDelta < 1e-9) {
      ++g_skip;
      std::printf(
          "    [SKIP] mask.apply 前置曲线：mask.apply 仍是 IdentityOp（M2 T2.6 实装），"
          "Δ 断言待 T2.6 接续；结构断言已在 O1-③ 通过（mask 行号 > 曲线行号）\n");
    } else {
      checkOrderSensitive("curve→mask（正确）vs 蒙版前置",
                          {Step{"curve.toneCurveRGB", curve}, Step{"mask.apply", maskParams}},
                          {Step{"mask.apply", maskParams}, Step{"curve.toneCurveRGB", curve}});
    }
  }
  // 组 3b：锐化前置于曲线（曲线位置的等价强证据；S4→S5 顺序被真实执行）
  {
    const JsonValue curve = curveParams("curve.toneCurveRGB");
    const JsonValue sharp = numParams("detail.sharpness", 80.0);
    checkOrderSensitive("curve→sharpen（正确）vs 锐化前置",
                        {Step{"curve.toneCurveRGB", curve}, Step{"detail.sharpen", sharp}},
                        {Step{"detail.sharpen", sharp}, Step{"curve.toneCurveRGB", curve}});
  }
  // 组 4（语义性组，架构 v1.1 §3.7）：exposure → highlights。
  // 这一组**不依赖 clamp**：highlights 的权重按像素亮度（t 域锚点）连续变化，
  // exposure 平移码值会改变该权重，因此两者在纯浮点域也不可交换。
  // 夹具刻意取低亮度区间（0.05..0.45）且断言「无样本到达码域上限」，
  // 用「clipped == 0 且 Δ ≥ 2」同时证明反例的语义性（不是靠裁切凑出来的）。
  {
    const ImageBuf low = makeRampFixture(192, 128, 0.05, 0.45);
    const JsonValue exposure = numParams("basic.exposure", 1.0);
    const JsonValue highlights = numParams("basic.highlights", -80.0);
    ImageBuf probe = low;
    std::string probeErr;
    const bool okProbe = runOne("basic.exposure", exposure, &probe, &probeErr);
    const size_t clipped = okProbe ? countClipped(probe) : 0;
    std::printf("    [probe] 语义性组夹具 clipped=%zu（必须为 0 才算不依赖 clamp）\n", clipped);
    CHECK(okProbe && clipped == 0, "语义性组夹具未触及码域上限（排除 clamp 依赖）");
    checkOrderSensitive("exposure→highlights（正确）vs 互换",
                        {Step{"basic.exposure", exposure}, Step{"basic.highlights", highlights}},
                        {Step{"basic.highlights", highlights}, Step{"basic.exposure", exposure}},
                        &low);
  }
  // 组 4b（实测发现，已上报架构）：锐化 ↔ 降噪**不能**作为语义性反例。
  // 现有实现里降噪是盒式模糊（线性）、锐化是反遮罩（线性），线性算子可交换，
  // 只在码域 clamp 处才分叉（实测 Δ = 0.46 LSB，远低于 2 LSB 门槛）。
  // 这里把「不成立」显式断言下来，防止后人误以为漏测而加一条假反例。
  //
  // 前置（架构 v1.2.0-M2-1.5 §3.7 裁定）：本负例成立的前提是**锐化走线性路径** ——
  // SharpenOp 在 masking>0 时带 |resid|<threshold 硬门（detail.cpp:116），非线性，
  // 线性可交换论证即失效。因此 masking 必须显式钉死为 0 并断言出来
  //（O2 纪律：反例成立的前提必须写进文档并断言出来，不能只写结论）。
  {
    JsonValue sharp = numParams("detail.sharpness", 80.0);
    sharp.setItem("detail.sharpenMasking", 0.0);  // 显式钉死线性路径，不依赖算子默认值
    // 前置断言：masking 必须 == 0。若将来有人在此夹具上启用 masking（或默认值变化），
    // 这条先红并明示「非线性前提被破坏」，而不是让下面的 Δ<2 断言假红或被误删。
    const JsonValue& maskingVal = sharp.get("detail.sharpenMasking");
    CHECK(maskingVal.isNumber() && std::fabs(maskingVal.asDouble() - 0.0) < 1e-9,
          "组 4b 前置：detail.sharpenMasking == 0（masking>0 时 SharpenOp 非线性，负例失效）");
    const ImageBuf noisy = makeNoisyFixture(192, 128);
    JsonValue denoise = JsonValue::objectValue();
    denoise.setItem("detail.colorNR", 100.0);
    denoise.setItem("detail.luminanceSmoothing", 100.0);
    ImageBuf a = noisy;
    ImageBuf b = noisy;
    std::string err;
    const bool ok = runSeq({Step{"detail.denoise", denoise}, Step{"detail.sharpen", sharp}}, &a,
                           &err) &&
                    runSeq({Step{"detail.sharpen", sharp}, Step{"detail.denoise", denoise}}, &b,
                           &err);
    const double d = ok ? maxAbsDeltaLsb(a, b) : 1e9;
    std::printf("    denoise↔sharpen 实测 max|Δ| = %.2f LSB（线性算子可交换，< 门槛属预期）\n", d);
    CHECK(ok && d < kOrderDeltaThresholdLsb,
          "锐化↔降噪是线性算子对：Δ < 2 LSB（不能当语义性反例，见报告）");
  }
}

// ---------------- O2 ①：clarity↔exposure 的 clamp 依赖 ----------------
// 架构 v1.1 §3.7 要求：这条反例必须**显式断言其 clamp 依赖**，否则将来 clamp
// 挪位时断言会静默失效，而人第一反应会去怀疑测试写错。做法：同一对算子、同一组
// 参数，只改夹具的亮度区间，让「是否发生码域裁切」成为唯一变量。
static void testClampDependency() {
  std::printf("O2-① clarity↔exposure 的 clamp 依赖（架构 v1.1 §3.7）\n");
  const JsonValue clarity = numParams("basic.clarity", 80.0);
  const JsonValue exposure = numParams("basic.exposure", 2.5);
  const std::vector<Step> correct{Step{"basic.clarity", clarity}, Step{"basic.exposure", exposure}};
  const std::vector<Step> swapped{Step{"basic.exposure", exposure}, Step{"basic.clarity", clarity}};

  struct Case {
    const char* name;
    double lo;
    double hi;
    bool expectClip;
  };
  const Case cases[2] = {{"无裁切（低亮度区间）", 0.05, 0.15, false},
                         {"有裁切（高光进入码域上限）", 0.10, 1.00, true}};
  for (const Case& c : cases) {
    ImageBuf a = makeRampFixture(192, 128, c.lo, c.hi);
    ImageBuf b = a;
    ImageBuf probe = a;  // 只施加 exposure，用来数裁切样本
    std::string err;
    if (!runOne("basic.exposure", exposure, &probe, &err)) {
      CHECK(false, (std::string(c.name) + "：probe 执行失败 " + err).c_str());
      continue;
    }
    const size_t clipped = countClipped(probe);
    if (!runSeq(correct, &a, &err) || !runSeq(swapped, &b, &err)) {
      CHECK(false, (std::string(c.name) + "：序列执行失败 " + err).c_str());
      continue;
    }
    const double d = maxAbsDeltaLsb(a, b);
    std::printf("    %-28s clipped=%-6zu max|Δ| = %.2f LSB\n", c.name, clipped, d);
    if (c.expectClip) {
      CHECK(clipped > 0, (std::string(c.name) + "：确实发生了码域裁切").c_str());
      CHECK(d >= kOrderDeltaThresholdLsb,
            (std::string(c.name) + "：换序差异 ≥ 2 LSB（依赖 clamp 成立）").c_str());
    } else {
      CHECK(clipped == 0, (std::string(c.name) + "：无样本到达码域上限").c_str());
      CHECK(d < kOrderDeltaThresholdLsb,
            (std::string(c.name) + "：无 clamp 时两者可交换（Δ < 2 LSB）").c_str());
    }
  }
}

// ---------------- O2 前置：编译出的顺序就是「正确顺序」 ----------------
static void testCompiledOrderMatchesO2() {
  std::printf("O2 前置：compilePlan 的相对顺序 = 反例中的「正确顺序」\n");
  const auto params = fullParams();
  lumina::plan::RenderPlan plan;
  std::string err;
  CHECK(lumina::plan::compilePlan(1, params, &plan, &err), "compilePlan 全参成功");
  if (g_fail) return;
  auto pos = [&plan](const std::string& op) -> int {
    for (size_t i = 0; i < plan.opSequence.size(); ++i) {
      if (plan.opSequence[i] == op) return static_cast<int>(i);
    }
    return -1;
  };
  CHECK(pos("basic.clarity") < pos("basic.exposure"), "clarity 先于 exposure");
  CHECK(pos("basic.exposure") < pos("basic.contrast"), "exposure 先于 contrast（S2 语义序，行 9→10）");
  CHECK(pos("pipeline.toneMap") < pos("curve.toneCurveRGB"), "toneMap 先于曲线");
  CHECK(pos("curve.toneCurveRGB") < pos("mask.apply"), "曲线先于蒙版");
  CHECK(pos("mask.apply") < pos("detail.sharpen"), "蒙版先于锐化");
}

int main() {
  std::string err;
  if (!lumina::plan::registerBuiltinOps(&OpTable::instance(), &err)) {
    std::printf("FATAL registerBuiltinOps: %s\n", err.c_str());
    return 2;
  }
  testPipelineOrderTable();
  testPlanSequenceOrder();
  testCompiledOrderMatchesO2();
  testOrderSensitiveCounterexamples();
  testClampDependency();
  // CI 可见性（架构 §3.7：M2 出口 = 零 SKIP）：SKIP 数以可解析格式打到 stdout，
  // 供 CI 统计；ctest 的 PASS 不代表 0 SKIP，必须看这一行。
  std::printf("[SKIP-COUNT] %d\n", g_skip);
  std::printf("\n%s (%d failures, %d skipped)\n", g_fail == 0 ? "ALL PASS" : "FAILED", g_fail,
              g_skip);
  return g_fail == 0 ? 0 : 1;
}
