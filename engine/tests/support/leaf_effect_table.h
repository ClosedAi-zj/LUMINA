// M2 T2.1 · 叶效果判据表（架构 §3.6 / §1.6，Q5）
//
// ============================================================================
//  ⚠ CI 只读：本文件的阈值只许收紧，不许下调（S10）。
//  开发侧为了让测试变绿而调低阈值 = 直接拒绝。需要放宽必须先改架构文档
//  （docs/v1.2.0/M2-架构设计.md §3.6）并走版本化，否则视同作弊。
//  收紧方向：kIncrease 行把 threshold 调大；kDecrease 行把 threshold 调更负；
//            kAtMost 行把 threshold 调小。
// ============================================================================
//
// 覆盖范围：M1 §五 K2 点名的 10 个「8bit 整幅均值不灵敏」的叶 + K3 的 dehaze。
//   K2：temperature / tint / vibrance / saturation / blueSat / clarity / texture /
//       sharpness / colorNR / grain
//   K3：dehaze
//
// 每行的 `step` = 本表定义的「单步」测试增量（默认参数 → 默认 + step），
// 取值为面板上的一次可用增量或该叶产生可辨效果所需的最小整量程增量（1/10/30/60/100），
// **不得为凑阈值反推小数**。
//
// ⚠ `step` 与 `threshold` 同等重要（架构 v1.1 R-1，S10 已扩到 step）：
//   改 step = 改门槛。想把某行「调绿」而去放大 step，和下调阈值是同一件事。
//   因此改 step 必须同时改架构文档 §3.6 并升版本号，否则视同作弊。
//   已知留痕：`optics.grainAmount` 的 step 由 20 调到 30（阈值 2% 未动）——
//   step=20 时实测 varRelChange = 1.68%（差 0.32pp），30 才是该叶在面板上
//   「一次可完成且必然可辨」的增量，不是为过线而调。
//
// 判据与实测值的对照在 test_leaf_effects 的输出里逐行打印，便于评审复核。
//
// 「生效」与「无操作」互斥（Q5 硬要求）：
//   assertEffective = 所有条件按方向成立（kIncrease ≥ t / kDecrease ≤ t / kAtMost ≤ t）
//   assertNoOp      = 所有条件都 ≤ 阈值 × kNoOpFactor（kDecrease 行是 ≥ t×0.5）
//   只要表里存在至少一条方向性条件，两个断言在数学上就不可能同时成立；
//   assertNoOp 内部还会再判一次 !assertEffective 兜底（防止将来有人加错行）。
#ifndef LUMINA_TESTS_SUPPORT_LEAF_EFFECT_TABLE_H_
#define LUMINA_TESTS_SUPPORT_LEAF_EFFECT_TABLE_H_

#include <cmath>
#include <cstddef>
#include <string>

#include "support/region_stats.h"

namespace lumina {
namespace testsupport {

// 判据可用的指标（全部来自同一份 LeafDelta）
enum class Metric {
  kNone = 0,
  kMaxCellLumDelta,        // 最坏格亮度差（0..255 尺度，无符号）
  kMaxCellChromaDelta,     // 最坏格色度模长差（Lab 单位，带符号）
  kMaxCellChromaDeltaAbs,  // 最坏格 Δ(a*,b*) 欧氏（Lab 单位，无符号）
  kVarRelChange,           // 16×16 块方差均值相对变化（带符号）
  kEdgeRelChange,          // Sobel 边缘能量均值相对变化（带符号）
  kQuartileLumDelta0,      // 最暗亮度四分位桶的亮度差（带符号，0..255 尺度）
  kGlobalMeanDelta,        // 整幅亮度均值变化（无符号，8bit LSB）
  kChromaVarRelChange,     // 全图色度方差相对变化（带符号）
};

// 方向：kAtMost 是「上界门限」（如 grain 的零均值），不承担判别方向。
enum class Dir {
  kNone = 0,
  kIncrease,  // 指标 ≥ 阈值
  kDecrease,  // 指标 ≤ 阈值（阈值为负）
  kAtMost,    // 指标 ≤ 阈值（门限）
};

struct LeafCriterion {
  const char* leaf = "";       // 叶路径（= 参数注入键，S1）
  const char* op = "";         // 承载该叶的算子名
  double step = 0.0;           // 「单步」测试增量
  Metric metric1 = Metric::kNone;
  double threshold1 = 0.0;
  Dir dir1 = Dir::kNone;
  Metric metric2 = Metric::kNone;   // kNone = 该行只有一个条件
  double threshold2 = 0.0;
  Dir dir2 = Dir::kNone;
  const char* note = "";
};

// 无操作反例的判定系数：全部指标 ≤ 阈值 × 0.5
constexpr double kNoOpFactor = 0.5;

// 11 行：K2 的 10 叶 + K3 的 dehaze
inline const LeafCriterion kLeafCriteria[] = {
    {"basic.temperature", "basic.temperature", 100.0,
     Metric::kMaxCellChromaDelta, 0.60, Dir::kIncrease,
     Metric::kNone, 0.0, Dir::kNone,
     "原 8bit 整幅均值 0.49 LSB 不灵敏；色温只动通道比例，整幅亮度几乎不变"},
    {"basic.tint", "basic.tint", 100.0,
     Metric::kMaxCellChromaDeltaAbs, 0.30, Dir::kIncrease,
     Metric::kNone, 0.0, Dir::kNone,
     "绿-品红轴：亮度变化被 L* 吸收，只有 a*/b* 能判。用欧氏幅度而非带符号 ΔC，"
     "因为 tint 是色相轴旋转，ΔC 符号随像素原色相变化（红黄区升、绿青区降）；"
     "方向正确性由 testHueAxisDirection 在近中性图上单独断言"},
    {"basic.vibrance", "basic.vibrance", 10.0,
     Metric::kMaxCellChromaDelta, 1.50, Dir::kIncrease,
     Metric::kNone, 0.0, Dir::kNone,
     "小面积效应：只抬低饱和像素，整幅均值被大面积中性区稀释"},
    {"basic.saturation", "basic.saturation", 10.0,
     Metric::kMaxCellChromaDelta, 1.50, Dir::kIncrease,
     Metric::kNone, 0.0, Dir::kNone,
     "保亮度算子：整幅亮度均值恒不变，只能看色度"},
    {"calibration.blueSat", "calibration.blue", 10.0,
     Metric::kMaxCellChromaDelta, 1.50, Dir::kIncrease,
     Metric::kNone, 0.0, Dir::kNone,
     "只作用于蓝通道离差，全图占比小"},
    {"basic.clarity", "basic.clarity", 10.0,
     Metric::kVarRelChange, 0.03, Dir::kIncrease,
     Metric::kNone, 0.0, Dir::kNone,
     "保均值算子：局部对比抬方差但不动均值（K2 的核心反例）"},
    {"basic.texture", "basic.texture", 10.0,
     Metric::kVarRelChange, 0.03, Dir::kIncrease,
     Metric::kNone, 0.0, Dir::kNone,
     "同上，半径更小（2px），吃高频"},
    {"detail.sharpness", "detail.sharpen", 10.0,
     Metric::kEdgeRelChange, 0.05, Dir::kIncrease,
     Metric::kNone, 0.0, Dir::kNone,
     "反遮罩锐化抬边缘梯度，均值不变"},
    {"detail.colorNR", "detail.denoise", 100.0,
     Metric::kVarRelChange, -0.10, Dir::kDecrease,
     Metric::kChromaVarRelChange, -0.10, Dir::kDecrease,
     "去噪必须降方差（方差降 = 生效）。色度均值对零均值色噪不灵敏，故看方差"},
    {"optics.grainAmount", "effects.grain", 30.0,
     Metric::kVarRelChange, 0.02, Dir::kIncrease,
     Metric::kGlobalMeanDelta, 1.0, Dir::kAtMost,
     "零均值噪声：方差升且均值必须 ≤1 LSB（两条同时成立才算对）"},
    {"basic.dehaze", "basic.dehaze", 60.0,
     Metric::kQuartileLumDelta0, 2.0, Dir::kIncrease,
     Metric::kNone, 0.0, Dir::kNone,
     "K3：整幅均值 1.5 LSB 被亮部稀释，最暗四分位桶才是判据"},
};

constexpr size_t kLeafCriteriaCount = sizeof(kLeafCriteria) / sizeof(kLeafCriteria[0]);

// 指标取值（Metric → LeafDelta 字段）
inline double metricValue(const LeafDelta& d, Metric m) {
  switch (m) {
    case Metric::kMaxCellLumDelta: return d.maxCellLumDelta;
    case Metric::kMaxCellChromaDelta: return d.maxCellChromaDelta;
    case Metric::kMaxCellChromaDeltaAbs: return d.maxCellChromaDeltaAbs;
    case Metric::kVarRelChange: return d.varRelChange;
    case Metric::kEdgeRelChange: return d.edgeRelChange;
    case Metric::kQuartileLumDelta0: return d.quartileLumDelta[0];
    case Metric::kGlobalMeanDelta: return d.globalMeanDelta;
    case Metric::kChromaVarRelChange: return d.chromaVarRelChange;
    case Metric::kNone: return 0.0;
  }
  return 0.0;
}

inline const char* metricName(Metric m) {
  switch (m) {
    case Metric::kMaxCellLumDelta: return "maxCellLumDelta";
    case Metric::kMaxCellChromaDelta: return "maxCellChromaDelta";
    case Metric::kMaxCellChromaDeltaAbs: return "maxCellChromaDeltaAbs";
    case Metric::kVarRelChange: return "varRelChange";
    case Metric::kEdgeRelChange: return "edgeRelChange";
    case Metric::kQuartileLumDelta0: return "quartileLumDelta[0]";
    case Metric::kGlobalMeanDelta: return "globalMeanDelta";
    case Metric::kChromaVarRelChange: return "chromaVarRelChange";
    case Metric::kNone: return "-";
  }
  return "-";
}

// 单条件判定。noOp = true 时按「阈值 × kNoOpFactor」的无操作口径判：
//   kIncrease：生效 v ≥ t；无操作 v ≤ t×0.5
//   kDecrease：生效 v ≤ t（t 为负）；无操作 v ≥ t×0.5（没降那么多）
//   kAtMost  ：生效 v ≤ t；无操作 v ≤ t×0.5
inline bool conditionHolds(double v, double threshold, Dir dir, bool noOp) {
  switch (dir) {
    case Dir::kIncrease:
      return noOp ? (v <= threshold * kNoOpFactor) : (v >= threshold);
    case Dir::kDecrease:
      return noOp ? (v >= threshold * kNoOpFactor) : (v <= threshold);
    case Dir::kAtMost:
      return noOp ? (v <= threshold * kNoOpFactor) : (v <= threshold);
    case Dir::kNone:
      return true;
  }
  return true;
}

// 生效：所有条件按方向成立
inline bool assertEffective(const LeafDelta& d, const LeafCriterion& c, std::string* why) {
  const double v1 = metricValue(d, c.metric1);
  if (!conditionHolds(v1, c.threshold1, c.dir1, false)) {
    if (why) {
      *why = std::string(c.leaf) + ": " + metricName(c.metric1) + "=" + std::to_string(v1) +
             " 未达阈值 " + std::to_string(c.threshold1) + "（方向 " +
             (c.dir1 == Dir::kIncrease ? "增" : (c.dir1 == Dir::kDecrease ? "减" : "上界")) + "）";
    }
    return false;
  }
  if (c.metric2 != Metric::kNone) {
    const double v2 = metricValue(d, c.metric2);
    if (!conditionHolds(v2, c.threshold2, c.dir2, false)) {
      if (why) {
        *why = std::string(c.leaf) + ": " + metricName(c.metric2) + "=" + std::to_string(v2) +
               " 未达阈值 " + std::to_string(c.threshold2);
      }
      return false;
    }
  }
  return true;
}

// 无操作：所有指标 ≤ 阈值 × 0.5，且必须与 assertEffective 互斥（内部兜底）
inline bool assertNoOp(const LeafDelta& d, const LeafCriterion& c, std::string* why) {
  if (assertEffective(d, c, nullptr)) {
    if (why) {
      *why = std::string(c.leaf) + ": 与 assertEffective 同时成立 —— 判据表被破坏（Q5 违约）";
    }
    return false;
  }
  const double v1 = metricValue(d, c.metric1);
  if (!conditionHolds(v1, c.threshold1, c.dir1, true)) {
    if (why) {
      *why = std::string(c.leaf) + ": 无操作反例失败，" + metricName(c.metric1) + "=" +
             std::to_string(v1) + " 超过阈值×0.5";
    }
    return false;
  }
  if (c.metric2 != Metric::kNone) {
    const double v2 = metricValue(d, c.metric2);
    if (!conditionHolds(v2, c.threshold2, c.dir2, true)) {
      if (why) {
        *why = std::string(c.leaf) + ": 无操作反例失败，" + metricName(c.metric2) + "=" +
               std::to_string(v2) + " 超过阈值×0.5";
      }
      return false;
    }
  }
  return true;
}

// 按叶路径查判据行（未登记返回 nullptr —— 调用方必须显式报错，不得静默跳过）
inline const LeafCriterion* findCriterion(const std::string& leaf) {
  for (size_t i = 0; i < kLeafCriteriaCount; ++i) {
    if (leaf == kLeafCriteria[i].leaf) return &kLeafCriteria[i];
  }
  return nullptr;
}

}  // namespace testsupport
}  // namespace lumina

#endif  // LUMINA_TESTS_SUPPORT_LEAF_EFFECT_TABLE_H_
