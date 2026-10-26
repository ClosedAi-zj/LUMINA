// M2 T2.1 · 分区统计器（架构 §1.6 / §3.6，Q5 口径先立）
//
// 为什么需要它（K2/K3 教训）：8bit 整幅均值对「保均值算子」（clarity/texture/
// sharpness/colorNR）、「零均值噪声」（grain）、「小面积效应」（vibrance/
// saturation/blueSat）、「设计值低于阈值」（temperature/tint）全都不灵敏。
// 本模块把「一幅图」拆成可比较的分区指标，让「生效」与「无操作」在同一口径下
// 互斥可判：
//   assertEffective(leaf) = 判据表中的指标超阈值（且方向正确）；
//   assertNoOp(leaf)      = 全部指标 ≤ 阈值 × 0.5；
// 二者由同一 RegionStats 差分产生，因此不可能同时成立（见 leaf_effect_table.h）。
//
// 度量口径（全部换算到「8bit 显示码 LSB」或「Lab 单位」，两者同为肉眼可辨量级）：
//   亮度   : CIE Lab 的 L* → lum = L*/100*255（0..255，1 单位 ≈ 1 个 8bit LSB）
//   色度   : CIE Lab 的 a*/b*（Lab 单位，由显示 sRGB 反算，D65 白点）
//   局部方差: 16×16 块方差的均值（三通道方差取平均；保均值算子靠它判定）
//   边缘能量: Sobel 幅值均值（亮度平面上求）
// 桶划分：亮度四分位桶按**基线图**的亮度排序切成 4 等份，cur 复用基线的桶归属
//         （保证比较的是同一批像素——这是 dehaze/highlights 类判据成立的前提）。
//
// 归属：engine/tests/support/**（仅测试工装；不得被 src/ 引用）。
#ifndef LUMINA_TESTS_SUPPORT_REGION_STATS_H_
#define LUMINA_TESTS_SUPPORT_REGION_STATS_H_

#include <cstdint>
#include <string>
#include <vector>

#include "imaging/image_buffer.h"
#include "imaging/imaging.h"

namespace lumina {
namespace testsupport {

// 5×5 网格中的一格（cells[gy*5+gx]）
struct Cell {
  double meanLum = 0.0;     // 0..255（L* 尺度）
  double meanA = 0.0;       // Lab 单位
  double meanB = 0.0;       // Lab 单位
  double varLocal = 0.0;    // 16×16 块方差均值（8bit 码² 尺度）
  double edgeEnergy = 0.0;  // Sobel 幅值均值（0..255 尺度）
  int count = 0;
};

// 亮度四分位桶（quartiles[0] = 最暗桶）
struct Bucket {
  double meanLum = 0.0;
  double meanA = 0.0;
  double meanB = 0.0;
  double varLocal = 0.0;
  double edgeEnergy = 0.0;
  int count = 0;
};

// 一幅图的完整分区统计。除 §3.6 规定的 cells/quartiles 外，额外保留逐像素平面：
// compareRegionStats 必须用「基线的桶归属」重新统计 cur，而 RegionStats 是唯一
// 能在 base/cur 之间传递该归属的载体（纯 cells/quartiles 无法复算）。
struct RegionStats {
  int w = 0;
  int h = 0;
  std::vector<Cell> cells;      // 25（5×5，行优先）
  std::vector<Bucket> quartiles;  // 4（按基线亮度升序）
  // ---- 实现补充：逐像素平面（供 compareRegionStats 复用基线桶归属）----
  std::vector<float> lumPlane;    // w*h，0..255
  std::vector<float> aPlane;      // w*h，Lab 单位
  std::vector<float> bPlane;      // w*h，Lab 单位
  std::vector<float> varPlane;    // w*h，所属 16×16 块的方差（块内同值）
  std::vector<float> edgePlane;   // w*h，Sobel 幅值
  std::vector<uint8_t> quartileOf;  // w*h，0..3（基线桶归属）
  // 色度局部方差平面（16×16 块内 a*/b* 方差之和）。去噪判据必须用**局部**色度方差：
  // 全图色度方差被大尺度色相结构（天空/肤色/植被）主导，零均值色噪只占很小比例，
  // 用它判 colorNR 会把「降噪 50%」看成「-0.7%」（K2 的同类陷阱）。
  std::vector<float> chromaVarPlane;
  double globalMeanLum = 0.0;     // 整幅亮度均值（0..255）
  double chromaVar = 0.0;         // 色度局部方差均值（a*/b* 块方差之和的均值）
};

// 差分结果（§3.6）。除规定字段外新增：
//   maxCellChromaDeltaAbs —— 无符号欧氏 Δ(a*,b*)，报告用；
//   chromaVarRelChange    —— 色度方差相对变化（带符号），colorNR 判据用
//                            （色度**均值**对零均值色噪不灵敏，ΔC≈0，故用方差）。
struct LeafDelta {
  double maxCellLumDelta = 0.0;       // 0..255 尺度（无符号，最坏格）
  double maxCellChromaDelta = 0.0;    // 带符号 ΔC（取 |ΔC| 最大的格，C=√(a*²+b*²)）
  double maxCellChromaDeltaAbs = 0.0; // 无符号欧氏 Δ(a*,b*)（最坏格）
  double varRelChange = 0.0;          // 相对变化，带符号
  double edgeRelChange = 0.0;         // 相对变化，带符号
  double quartileLumDelta[4] = {0.0, 0.0, 0.0, 0.0};  // 带符号（0..255 尺度）
  double globalMeanDelta = 0.0;       // 整幅亮度均值变化，无符号（8bit LSB）
  double globalMaxDelta = 0.0;        // 逐像素亮度最大变化，无符号（8bit LSB）
  double chromaVarRelChange = 0.0;    // 色度方差相对变化，带符号
  int worstCell = -1;                 // 亮度差最大的格下标（0..24）
};

// 基线统计：按 base 自身的亮度分位数切 4 桶，并记录逐像素桶归属。
// 8bit 版（§3.6 逐字签名）：显示 sRGB 8bit 直方图/PNG 输入走这条。
bool computeRegionStats(const imaging::Image& base, RegionStats* out, std::string* err);
// 16bit 版（测试主用）：ImageBuf 的 code/65535 视为显示值（S2 域约定），
// 避免 8bit 量化把 clarity/texture 的 3% 方差变化吃掉。
bool computeRegionStatsBuf(const imaging::ImageBuf& base, RegionStats* out, std::string* err);

// base 固定网格与桶划分，cur 用同一划分（保证「同一批像素」比较）。
bool compareRegionStats(const RegionStats& base, const RegionStats& cur, LeafDelta* out);

}  // namespace testsupport
}  // namespace lumina

#endif  // LUMINA_TESTS_SUPPORT_REGION_STATS_H_
