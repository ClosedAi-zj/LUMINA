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
// 注意（实测发现，已上报架构）：
//   - clarity 与 exposure 在**无裁切**时严格可交换（clarity = u + k(u-B)，B 是线性
//     平移不变的盒式模糊；exposure = u + 常数），因此该组的 Δ 只能来自码域 clamp。
//     本测试用「高光进入 65535 裁切」的参数让顺序差异真实可测，并另加一组
//     contrast↔exposure（仿射 vs 平移，天然不可交换）作为无裁切下的强证据。
//   - mask.apply 在 T2.6 之前仍是 IdentityOp，因此「蒙版前置于曲线」的 Δ 断言
//     暂记 SKIP（并打印 T2.6 待接续）；其**结构**断言（mask.apply 行号 > 曲线行号）
//     现在就成立。为避免这条验收变成空门，另加一组 sharpen↔curve 的真实 Δ 证据。
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
                                const std::vector<Step>& wrong) {
  const ImageBuf base = makeOrderFixture(192, 128);
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
  std::printf("\n%s (%d failures, %d skipped)\n", g_fail == 0 ? "ALL PASS" : "FAILED", g_fail,
              g_skip);
  return g_fail == 0 ? 0 : 1;
}
