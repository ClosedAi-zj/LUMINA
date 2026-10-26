// M2 T2.1 验收：K2/K3 新口径（分区统计 + 判据表）落地断言
//
// 断言对象（PRD §6.6 / 架构 §1.6 / §1.7 遗留 #3）：
//   K2 的 10 叶 + K3 的 dehaze，在新口径下「全部可判定生效且方向正确」，
//   同时「无操作反例」成立（全部指标 ≤ 阈值 × 0.5），两者互斥。
//
// 反例为什么必须存在（M1 K1 教训）：只断言「生效」会漏掉「一致地错」——
// 一个把参数当噪声源随便加点东西的算子也能过。因此每叶都要跑两遍：
//   ① 参数 = 默认值 → 必须 assertNoOp（真的什么都不做）
//   ② 参数 = 默认 + step → 必须 assertEffective 且方向正确
// ① 与 ② 由同一 RegionStats 差分产生，口径完全一致。
//
// 8bit 整幅均值口径保留为回归项（不删，但不再是唯一判据）：见 testLegacyMeanRegression。
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
#include "support/leaf_effect_table.h"
#include "support/region_stats.h"

using namespace lumina;
using lumina::imaging::ImageBuf;
using lumina::json::JsonValue;
using lumina::plan::OpTable;
using lumina::testsupport::LeafCriterion;
using lumina::testsupport::LeafDelta;
using lumina::testsupport::RegionStats;

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

namespace {

// 判据夹具：一幅图同时满足 11 行的取材需求（确定性、无随机源）：
//   ① 水平亮度斜坡 0.06..0.68  → 亮度四分位桶有明显分层（dehaze 的最暗桶）
//   ② 色相沿 x 平滑扫过整圈      → vibrance/saturation/blueSat 有可测色度；
//      必须**平滑**：色块硬边界会让 16×16 块方差被「模糊抹平边界」主导，
//      从而把 colorNR 的降噪误读成升方差（本夹具首版就踩过这个坑）
//   ③ 高频细纹（周期 3px）      → clarity/texture/sharpness 有残差可放大
//   ④ 逐像素色噪（确定性哈希）  → colorNR 有局部色度方差可降
// 全部直接写码空间（code = v*65535），与 test_ops93 的探针口径一致。
ImageBuf makeCriteriaFixture(int w, int h) {
  ImageBuf buf(w, h);
  constexpr double kTwoPi = 6.283185307179586;
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      const double t = (w > 1) ? static_cast<double>(x) / (w - 1) : 0.0;
      const double ramp = 0.06 + 0.62 * t;  // ① 亮度斜坡
      // ② 平滑色相扫（叠加 y 向轻微摆动，避免整幅退化成一条色带）
      double hue = t + 0.12 * std::sin(kTwoPi * static_cast<double>(y) / std::max(1, h));
      hue -= std::floor(hue);
      // 饱和度沿 y 从近中性(0.06)扫到饱和(0.61)：
      //   temperature/tint 在近中性处 a*/b* 灵敏度最高（这就是它们的判据来源），
      //   vibrance/saturation/blueSat 需要饱和区（近中性处离差为 0，本来就该判不出）。
      const double sat = 0.06 + 0.55 * (h > 1 ? static_cast<double>(y) / (h - 1) : 0.0);
      double cr = 0.5, cg = 0.5, cb = 0.5;
      lumina::plan::hslToRgb(hue, sat, 0.5, &cr, &cg, &cb);
      // 色度只改通道比例、几乎不动平均亮度（系数均值 ≈ 1.0），保住亮度斜坡分层
      double r = ramp * (0.4 + 1.2 * cr);
      double g = ramp * (0.4 + 1.2 * cg);
      double b = ramp * (0.4 + 1.2 * cb);
      // ③ 高频细纹：x 向周期 3px、y 向周期 3px
      const double texX = ((x % 3) - 1) * 0.010;
      const double texY = ((y % 3) - 1) * 0.005;
      const double tex = texX + texY;
      // ④ 逐像素色噪（零均值、确定性）
      const double nR = (lumina::plan::hashNoise(x, y, 11) - 0.5) * 0.06;
      const double nG = (lumina::plan::hashNoise(x, y, 37) - 0.5) * 0.03;
      const double nB = (lumina::plan::hashNoise(x, y, 23) - 0.5) * 0.06;
      r += tex + nR;
      g += tex + nG;
      b += tex + nB;
      const double v[3] = {r, g, b};
      uint16_t* p = &buf.data[(static_cast<size_t>(y) * w + x) * 3];
      for (int c = 0; c < 3; ++c) {
        double q = v[c];
        if (q < 0.0) q = 0.0;
        if (q > 1.0) q = 1.0;
        p[c] = static_cast<uint16_t>(q * 65535.0 + 0.5);
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

bool applyOp(const char* opName, const JsonValue& params, ImageBuf* buf, std::string* err) {
  const lumina::plan::Operator* op = OpTable::instance().find(opName);
  if (!op) {
    if (err) *err = std::string("op not found: ") + opName;
    return false;
  }
  std::vector<std::string> names{opName};
  std::map<std::string, JsonValue> byOp{{opName, params}};
  return OpTable::instance().evaluate(names, buf, byOp, err);
}

// 旧口径（8bit 整幅均值，单位 LSB）——保留为回归项，只打印不设门槛
double legacyMeanDeltaLsb(const ImageBuf& a, const ImageBuf& b) {
  double sum = 0.0;
  size_t n = 0;
  for (size_t i = 0; i + 2 < a.data.size(); i += 3) {
    const double la = (a.data[i] + a.data[i + 1] + a.data[i + 2]) / 3.0;
    const double lb = (b.data[i] + b.data[i + 1] + b.data[i + 2]) / 3.0;
    sum += std::fabs(lb - la) / 256.0;
    ++n;
  }
  return n ? sum / static_cast<double>(n) : 0.0;
}

}  // namespace

// ---- 0. 统计器自身的结构自检 ----
static void testStatsShape() {
  std::printf("testStatsShape（25 格 + 4 桶 + 桶按亮度升序）\n");
  const ImageBuf img = makeCriteriaFixture(160, 120);
  RegionStats st;
  std::string err;
  CHECK(lumina::testsupport::computeRegionStatsBuf(img, &st, &err), "computeRegionStatsBuf ok");
  CHECK(st.cells.size() == 25, "5×5 = 25 格");
  CHECK(st.quartiles.size() == 4, "亮度四分位 = 4 桶");
  int total = 0;
  for (const auto& c : st.cells) total += c.count;
  CHECK(total == st.w * st.h, "25 格像素数之和 = 全图像素数");
  bool monotonic = true;
  int btotal = 0;
  for (int q = 0; q < 4; ++q) {
    btotal += st.quartiles[static_cast<size_t>(q)].count;
    if (q > 0 && st.quartiles[static_cast<size_t>(q)].meanLum <
                     st.quartiles[static_cast<size_t>(q - 1)].meanLum) {
      monotonic = false;
    }
  }
  CHECK(monotonic, "桶 0..3 的平均亮度单调递增（桶 0 = 最暗）");
  CHECK(btotal == st.w * st.h, "4 桶像素数之和 = 全图像素数");
  std::printf("    base: meanLum=%.2f chromaVar=%.1f 桶亮度=%.1f/%.1f/%.1f/%.1f\n",
              st.globalMeanLum, st.chromaVar, st.quartiles[0].meanLum, st.quartiles[1].meanLum,
              st.quartiles[2].meanLum, st.quartiles[3].meanLum);
  // 8bit 版（§3.6 逐字签名）也必须可用
  lumina::imaging::Image img8;
  img8.width = img.w;
  img8.height = img.h;
  img8.pixels.resize(static_cast<size_t>(img.w) * img.h * 3);
  for (size_t i = 0; i < img8.pixels.size(); ++i) {
    img8.pixels[i] = static_cast<uint8_t>(img.data[i] / 256);
  }
  RegionStats st8;
  CHECK(lumina::testsupport::computeRegionStats(img8, &st8, &err), "computeRegionStats(8bit) ok");
  CHECK(st8.cells.size() == 25 && st8.quartiles.size() == 4, "8bit 版同为 25 格 + 4 桶");
}

// ---- 1. K2 的 10 叶 + K3 的 dehaze：生效 + 方向正确 ----
static void testLeafCriteriaEffective() {
  std::printf("testLeafCriteriaEffective（K2 10 叶 + K3 dehaze，新口径）\n");
  const ImageBuf base = makeCriteriaFixture(256, 192);
  RegionStats baseStats;
  std::string err;
  if (!lumina::testsupport::computeRegionStatsBuf(base, &baseStats, &err)) {
    CHECK(false, ("base stats failed: " + err).c_str());
    return;
  }
  std::printf("    叶 / step / 指标 / 实测 / 阈值\n");
  for (size_t i = 0; i < lumina::testsupport::kLeafCriteriaCount; ++i) {
    const LeafCriterion& c = lumina::testsupport::kLeafCriteria[i];
    // ① 默认参数 → 无操作反例
    ImageBuf noop = base;
    std::string aerr;
    const bool appliedNoop = applyOp(c.op, numParams(c.leaf, 0.0), &noop, &aerr);
    CHECK(appliedNoop, (std::string("apply@default: ") + c.op).c_str());
    RegionStats noopStats;
    LeafDelta noopDelta;
    if (appliedNoop && lumina::testsupport::computeRegionStatsBuf(noop, &noopStats, &err) &&
        lumina::testsupport::compareRegionStats(baseStats, noopStats, &noopDelta)) {
      std::string why;
      const bool ok = lumina::testsupport::assertNoOp(noopDelta, c, &why);
      CHECK(ok, (std::string("assertNoOp: ") + c.leaf +
                 (ok ? std::string() : std::string(" | ") + why)).c_str());
    } else {
      CHECK(false, (std::string("noop stats failed: ") + c.leaf).c_str());
    }
    // ② 默认 + step → 生效且方向正确
    ImageBuf step = base;
    const bool appliedStep = applyOp(c.op, numParams(c.leaf, c.step), &step, &aerr);
    CHECK(appliedStep, (std::string("apply@step: ") + c.op).c_str());
    RegionStats stepStats;
    LeafDelta d;
    if (appliedStep && lumina::testsupport::computeRegionStatsBuf(step, &stepStats, &err) &&
        lumina::testsupport::compareRegionStats(baseStats, stepStats, &d)) {
      std::string why;
      const bool ok = lumina::testsupport::assertEffective(d, c, &why);
      CHECK(ok, (std::string("assertEffective: ") + c.leaf +
                 (ok ? std::string() : std::string(" | ") + why)).c_str());
      const double v1 = lumina::testsupport::metricValue(d, c.metric1);
      std::printf("    %-24s step=%-5.0f %-22s %10.4f  thr=%.4f\n", c.leaf, c.step,
                  lumina::testsupport::metricName(c.metric1), v1, c.threshold1);
      if (c.metric2 != lumina::testsupport::Metric::kNone) {
        std::printf("    %-24s %-28s %-22s %10.4f  thr=%.4f\n", "", "(第二条件)",
                    lumina::testsupport::metricName(c.metric2),
                    lumina::testsupport::metricValue(d, c.metric2), c.threshold2);
      }
      std::printf("    %-24s 旧口径 8bit 整幅均值 Δ=%.3f LSB（回归项，不设门槛）\n", "",
                  legacyMeanDeltaLsb(base, step));
    } else {
      CHECK(false, (std::string("step stats failed: ") + c.leaf).c_str());
    }
  }
}

// ---- 1b. 色相轴语义方向（近中性图上直接断言 a*/b* 的符号） ----
// 为什么单独做：判据表里的色度指标是**幅度**，不含符号（色相旋转的符号随原色相变化）。
// 「方向正确」这一条在近中性图上是最干净的：暖 → a* 升；品红 → a* 升。
static void testHueAxisDirection() {
  std::printf("testHueAxisDirection（temperature/tint 的 a*/b* 方向）\n");
  // 近中性灰（三通道等值），色度接近 0，任何色度偏移都能读出符号
  ImageBuf neutral(64, 64);
  for (size_t i = 0; i < neutral.data.size(); ++i) neutral.data[i] = 29491;  // 0.45
  auto meanA = [](const ImageBuf& img) {
    RegionStats st;
    std::string e;
    if (!lumina::testsupport::computeRegionStatsBuf(img, &st, &e)) return 0.0;
    double s = 0.0;
    for (const auto& c : st.cells) s += c.meanA;
    return s / static_cast<double>(st.cells.size());
  };
  auto meanB = [](const ImageBuf& img) {
    RegionStats st;
    std::string e;
    if (!lumina::testsupport::computeRegionStatsBuf(img, &st, &e)) return 0.0;
    double s = 0.0;
    for (const auto& c : st.cells) s += c.meanB;
    return s / static_cast<double>(st.cells.size());
  };
  const double a0 = meanA(neutral);
  const double b0 = meanB(neutral);
  std::string err;
  ImageBuf warm = neutral;
  CHECK(applyOp("basic.temperature", numParams("basic.temperature", 50.0), &warm, &err),
        "apply temperature +50");
  std::printf("    temperature +50: a* %+.3f → %+.3f\n", a0, meanA(warm));
  CHECK(meanA(warm) > a0 + 0.05, "temperature>0（暖）→ a* 升（红多蓝少）");
  ImageBuf cool = neutral;
  CHECK(applyOp("basic.temperature", numParams("basic.temperature", -50.0), &cool, &err),
        "apply temperature -50");
  CHECK(meanA(cool) < a0 - 0.05, "temperature<0（冷）→ a* 降");
  ImageBuf magenta = neutral;
  CHECK(applyOp("basic.tint", numParams("basic.tint", 50.0), &magenta, &err), "apply tint +50");
  std::printf("    tint +50: a* %+.3f → %+.3f, b* %+.3f → %+.3f\n", a0, meanA(magenta), b0,
              meanB(magenta));
  CHECK(meanA(magenta) > a0 + 0.05, "tint>0（品红）→ a* 升");
  ImageBuf green = neutral;
  CHECK(applyOp("basic.tint", numParams("basic.tint", -50.0), &green, &err), "apply tint -50");
  CHECK(meanA(green) < a0 - 0.05, "tint<0（绿）→ a* 降");
}

// ---- 2. 互斥性：同一 LeafDelta 不可能同时满足两个断言 ----
static void testMutualExclusion() {
  std::printf("testMutualExclusion（Q5 硬要求：生效与无操作互斥）\n");
  for (size_t i = 0; i < lumina::testsupport::kLeafCriteriaCount; ++i) {
    const LeafCriterion& c = lumina::testsupport::kLeafCriteria[i];
    LeafDelta d;
    // 把第一条件推到「刚好达标」的位置
    const double v = c.threshold1;
    switch (c.metric1) {
      case lumina::testsupport::Metric::kMaxCellLumDelta: d.maxCellLumDelta = v; break;
      case lumina::testsupport::Metric::kMaxCellChromaDelta: d.maxCellChromaDelta = v; break;
      case lumina::testsupport::Metric::kMaxCellChromaDeltaAbs: d.maxCellChromaDeltaAbs = v; break;
      case lumina::testsupport::Metric::kVarRelChange: d.varRelChange = v; break;
      case lumina::testsupport::Metric::kEdgeRelChange: d.edgeRelChange = v; break;
      case lumina::testsupport::Metric::kQuartileLumDelta0: d.quartileLumDelta[0] = v; break;
      case lumina::testsupport::Metric::kGlobalMeanDelta: d.globalMeanDelta = v; break;
      case lumina::testsupport::Metric::kChromaVarRelChange: d.chromaVarRelChange = v; break;
      case lumina::testsupport::Metric::kNone: break;
    }
    if (c.metric2 != lumina::testsupport::Metric::kNone) {
      const double v2 = c.threshold2;
      switch (c.metric2) {
        case lumina::testsupport::Metric::kGlobalMeanDelta: d.globalMeanDelta = v2; break;
        case lumina::testsupport::Metric::kChromaVarRelChange: d.chromaVarRelChange = v2; break;
        case lumina::testsupport::Metric::kVarRelChange: d.varRelChange = v2; break;
        default: break;
      }
    }
    const bool eff = lumina::testsupport::assertEffective(d, c, nullptr);
    const bool noop = lumina::testsupport::assertNoOp(d, c, nullptr);
    CHECK(eff && !noop, (std::string("阈值点：assertEffective 成立且 assertNoOp 不成立: ") + c.leaf)
                            .c_str());
  }
}

// ---- 3. 旧口径回归项：8bit 整幅均值仍在，且证实它对保均值算子不灵敏 ----
// 目的不是设门槛，而是把 K2 的「为什么不换口径」钉在代码里：
// clarity/texture/sharpness/colorNR/grain 的整幅均值变化远小于 1 LSB，
// 旧口径必然漏判；新口径（方差/色度）能判。
static void testLegacyMeanRegression() {
  std::printf("testLegacyMeanRegression（旧 8bit 整幅均值口径保留 + 不灵敏性留证）\n");
  const ImageBuf base = makeCriteriaFixture(256, 192);
  const char* meanPreserving[] = {"basic.clarity", "basic.texture", "detail.sharpen",
                                  "detail.denoise", "effects.grain"};
  const char* leafKeys[] = {"basic.clarity", "basic.texture", "detail.sharpness",
                            "detail.colorNR", "optics.grainAmount"};
  for (int i = 0; i < 5; ++i) {
    const LeafCriterion* c = lumina::testsupport::findCriterion(leafKeys[i]);
    if (!c) {
      CHECK(false, (std::string("criterion missing: ") + leafKeys[i]).c_str());
      continue;
    }
    ImageBuf cur = base;
    std::string err;
    CHECK(applyOp(meanPreserving[i], numParams(c->leaf, c->step), &cur, &err),
          (std::string("apply: ") + meanPreserving[i]).c_str());
    const double legacy = legacyMeanDeltaLsb(base, cur);
    RegionStats a, b;
    LeafDelta d;
    if (lumina::testsupport::computeRegionStatsBuf(base, &a, &err) &&
        lumina::testsupport::computeRegionStatsBuf(cur, &b, &err) &&
        lumina::testsupport::compareRegionStats(a, b, &d)) {
      std::printf("    %-20s 旧口径 Δmean=%.3f LSB | 新口径 varRel=%+.2f%% edgeRel=%+.2f%%\n",
                  meanPreserving[i], legacy, d.varRelChange * 100.0, d.edgeRelChange * 100.0);
      // 旧口径对 grain 之外的四类必须「几乎无感」（< 1 LSB）——这是换口径的依据
      if (std::string(meanPreserving[i]) != "effects.grain") {
        CHECK(legacy < 1.0,
              (std::string("旧口径对该叶不灵敏（<1 LSB）: ") + meanPreserving[i]).c_str());
      }
    } else {
      CHECK(false, (std::string("stats failed: ") + meanPreserving[i]).c_str());
    }
  }
}

int main() {
  std::string err;
  if (!lumina::plan::registerBuiltinOps(&OpTable::instance(), &err)) {
    std::printf("FAIL registerBuiltinOps: %s\n", err.c_str());
    return 1;
  }
  testStatsShape();
  testLeafCriteriaEffective();
  testHueAxisDirection();
  testMutualExclusion();
  testLegacyMeanRegression();
  std::printf("\n%s (%d failures)\n", g_fail == 0 ? "ALL PASS" : "FAILED", g_fail);
  return g_fail == 0 ? 0 : 1;
}
