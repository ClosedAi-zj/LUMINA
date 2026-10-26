// M2 T2.1 · 分区统计器实现（架构 §3.6）
// 实现要点：
//  1) 逐像素先算 4 个平面（L*/a*/b*、16×16 块方差、Sobel 边缘幅值），
//     之后所有分区指标都由平面聚合而来 —— 保证 cells / quartiles / 整幅均值
//     三套口径永远来自同一批像素，不会出现「分区说生效、均值说没变」的分叉。
//  2) 桶归属（quartileOf）由基线图决定并随 RegionStats 传递，cur 复用同一归属，
//     因此 dehaze 的「最暗桶」在两个时刻是**同一批像素**（否则亮桶暗桶互相渗透，
//     判据会被稀释——这正是 M1 K3 的失败原因）。
//  3) 全部为确定性运算（无随机、无平台相关分支），阈值可跨机器比较。
#include "support/region_stats.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <numeric>
#include <vector>

namespace lumina {
namespace testsupport {
namespace {

constexpr int kGridCols = 5;
constexpr int kGridRows = 5;
constexpr int kCellCount = kGridCols * kGridRows;
constexpr int kQuartiles = 4;
constexpr int kVarBlock = 16;  // 局部方差块边长（架构 §1.6：16×16）

// 8bit 显示值 → 线性光（sRGB 传输曲线，D65）
inline double srgbToLinear(double v) {
  if (v <= 0.0) return 0.0;
  if (v <= 0.04045) return v / 12.92;
  return std::pow((v + 0.055) / 1.055, 2.4);
}

// CIE Lab 的 f(t)
inline double labF(double t) {
  constexpr double kEps = 216.0 / 24389.0;   // 0.008856
  constexpr double kKappa = 24389.0 / 27.0;  // 903.3
  if (t > kEps) return std::cbrt(t);
  return (kKappa * t + 16.0) / 116.0;
}

// 显示 sRGB（0..255）→ CIE Lab（D65 白点）。输出 L*∈[0,100]、a*/b*≈[-128,127]。
void srgbToLab(double r8, double g8, double b8, double* L, double* A, double* B) {
  const double r = srgbToLinear(r8 / 255.0);
  const double g = srgbToLinear(g8 / 255.0);
  const double b = srgbToLinear(b8 / 255.0);
  // sRGB → XYZ（D65，IEC 61966-2-1 矩阵）
  const double X = 0.4124564 * r + 0.3575761 * g + 0.1804375 * b;
  const double Y = 0.2126729 * r + 0.7151522 * g + 0.0721750 * b;
  const double Z = 0.0193339 * r + 0.1191920 * g + 0.9503041 * b;
  constexpr double kXn = 0.95047;
  constexpr double kYn = 1.00000;
  constexpr double kZn = 1.08883;
  const double fx = labF(X / kXn);
  const double fy = labF(Y / kYn);
  const double fz = labF(Z / kZn);
  *L = 116.0 * fy - 16.0;
  *A = 500.0 * (fx - fy);
  *B = 200.0 * (fy - fz);
}

// 单通道平面方差（两遍法，避免 E[x²]-E[x]² 的抵消误差）
double planeVariance(const std::vector<float>& v, const std::vector<size_t>& idx) {
  if (idx.empty()) return 0.0;
  double s = 0.0;
  for (size_t i : idx) s += v[i];
  const double mean = s / static_cast<double>(idx.size());
  double acc = 0.0;
  for (size_t i : idx) {
    const double d = static_cast<double>(v[i]) - mean;
    acc += d * d;
  }
  return acc / static_cast<double>(idx.size());
}

// 整幅平面方差（全体像素）
double planeVarianceAll(const std::vector<float>& v) {
  if (v.empty()) return 0.0;
  double s = 0.0;
  for (float x : v) s += x;
  const double mean = s / static_cast<double>(v.size());
  double acc = 0.0;
  for (float x : v) {
    const double d = static_cast<double>(x) - mean;
    acc += d * d;
  }
  return acc / static_cast<double>(v.size());
}

// 整幅平面均值（varLocal / edgeEnergy 的「和 / 像素数」口径，架构 §1.6）
double planeMean(const std::vector<float>& v) {
  if (v.empty()) return 0.0;
  double s = 0.0;
  for (float x : v) s += x;
  return s / static_cast<double>(v.size());
}

// 16×16 块方差平面（可加权的多平面求和）。planes 中每个平面各自求块方差后按权重相加，
// 块内所有像素同值。
void computeBlockVarPlane(const std::vector<const std::vector<float>*>& planes,
                          const std::vector<double>& weights, int w, int h,
                          std::vector<float>* out) {
  out->assign(static_cast<size_t>(w) * h, 0.0f);
  for (int by = 0; by < h; by += kVarBlock) {
    for (int bx = 0; bx < w; bx += kVarBlock) {
      const int x1 = std::min(w, bx + kVarBlock);
      const int y1 = std::min(h, by + kVarBlock);
      std::vector<size_t> idx;
      idx.reserve(static_cast<size_t>(x1 - bx) * (y1 - by));
      for (int y = by; y < y1; ++y) {
        for (int x = bx; x < x1; ++x) idx.push_back(static_cast<size_t>(y) * w + x);
      }
      double v = 0.0;
      for (size_t k = 0; k < planes.size(); ++k) {
        v += weights[k] * planeVariance(*planes[k], idx);
      }
      for (size_t i : idx) (*out)[i] = static_cast<float>(v);
    }
  }
}

// Sobel 边缘幅值平面（在亮度平面上求；边界像素记 0）
void computeEdgePlane(const std::vector<float>& lum, int w, int h, std::vector<float>* out) {
  out->assign(static_cast<size_t>(w) * h, 0.0f);
  if (w < 3 || h < 3) return;
  for (int y = 1; y < h - 1; ++y) {
    for (int x = 1; x < w - 1; ++x) {
      const size_t p = static_cast<size_t>(y) * w + x;
      const double p00 = lum[p - w - 1], p01 = lum[p - w], p02 = lum[p - w + 1];
      const double p10 = lum[p - 1], p12 = lum[p + 1];
      const double p20 = lum[p + w - 1], p21 = lum[p + w], p22 = lum[p + w + 1];
      const double gx = (p02 + 2.0 * p12 + p22) - (p00 + 2.0 * p10 + p20);
      const double gy = (p20 + 2.0 * p21 + p22) - (p00 + 2.0 * p01 + p02);
      (*out)[p] = static_cast<float>(std::sqrt(gx * gx + gy * gy));
    }
  }
}

// 网格边界：5×5 恰好铺满（整数分块，避免取整后留缝或重叠）
inline int gridBoundary(int i, int n) { return i * n / kGridCols; }

// 用给定的桶归属聚合（cur 复用 base 的归属时也走这里）
void aggregateBuckets(const RegionStats& s, const std::vector<uint8_t>& assign,
                      std::vector<Bucket>* out) {
  out->assign(kQuartiles, Bucket{});
  const size_t n = static_cast<size_t>(s.w) * s.h;
  for (size_t i = 0; i < n; ++i) {
    const int q = static_cast<int>(assign[i]);
    if (q < 0 || q >= kQuartiles) continue;
    Bucket& b = (*out)[q];
    b.meanLum += s.lumPlane[i];
    b.meanA += s.aPlane[i];
    b.meanB += s.bPlane[i];
    b.varLocal += s.varPlane[i];
    b.edgeEnergy += s.edgePlane[i];
    ++b.count;
  }
  for (Bucket& b : *out) {
    if (b.count <= 0) continue;
    const double inv = 1.0 / static_cast<double>(b.count);
    b.meanLum *= inv;
    b.meanA *= inv;
    b.meanB *= inv;
    b.varLocal *= inv;
    b.edgeEnergy *= inv;
  }
}

void aggregateCells(const RegionStats& s, std::vector<Cell>* out) {
  out->assign(kCellCount, Cell{});
  for (int gy = 0; gy < kGridRows; ++gy) {
    for (int gx = 0; gx < kGridCols; ++gx) {
      const int x0 = gridBoundary(gx, s.w);
      const int x1 = gridBoundary(gx + 1, s.w);
      const int y0 = gridBoundary(gy, s.h);
      const int y1 = gridBoundary(gy + 1, s.h);
      Cell& c = (*out)[gy * kGridCols + gx];
      for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) {
          const size_t i = static_cast<size_t>(y) * s.w + x;
          c.meanLum += s.lumPlane[i];
          c.meanA += s.aPlane[i];
          c.meanB += s.bPlane[i];
          c.varLocal += s.varPlane[i];
          c.edgeEnergy += s.edgePlane[i];
          ++c.count;
        }
      }
      if (c.count > 0) {
        const double inv = 1.0 / static_cast<double>(c.count);
        c.meanLum *= inv;
        c.meanA *= inv;
        c.meanB *= inv;
        c.varLocal *= inv;
        c.edgeEnergy *= inv;
      }
    }
  }
}

// 由三通道平面（0..255 尺度）构建完整统计
bool computeFromPlanes(const std::vector<float>& r, const std::vector<float>& g,
                       const std::vector<float>& b, int w, int h, RegionStats* out,
                       std::string* err) {
  if (!out) {
    if (err) *err = "region_stats: null out";
    return false;
  }
  if (w <= 0 || h <= 0) {
    if (err) *err = "region_stats: empty image";
    return false;
  }
  const size_t n = static_cast<size_t>(w) * h;
  if (r.size() != n || g.size() != n || b.size() != n) {
    if (err) *err = "region_stats: plane size mismatch";
    return false;
  }
  out->w = w;
  out->h = h;
  out->lumPlane.assign(n, 0.0f);
  out->aPlane.assign(n, 0.0f);
  out->bPlane.assign(n, 0.0f);
  for (size_t i = 0; i < n; ++i) {
    double L = 0.0, A = 0.0, B = 0.0;
    srgbToLab(r[i], g[i], b[i], &L, &A, &B);
    out->lumPlane[i] = static_cast<float>(L / 100.0 * 255.0);
    out->aPlane[i] = static_cast<float>(A);
    out->bPlane[i] = static_cast<float>(B);
  }
  // 通道方差（保均值算子判据）：三通道块方差取平均
  computeBlockVarPlane({&r, &g, &b}, {1.0 / 3.0, 1.0 / 3.0, 1.0 / 3.0}, w, h, &out->varPlane);
  computeEdgePlane(out->lumPlane, w, h, &out->edgePlane);

  // 亮度四分位桶：按基线亮度升序切 4 等份（桶集合由基线固定）
  std::vector<size_t> order(n);
  std::iota(order.begin(), order.end(), static_cast<size_t>(0));
  std::stable_sort(order.begin(), order.end(), [out](size_t a, size_t b) {
    return out->lumPlane[a] < out->lumPlane[b];
  });
  out->quartileOf.assign(n, 0);
  for (size_t rank = 0; rank < n; ++rank) {
    int q = static_cast<int>(rank * kQuartiles / n);
    if (q < 0) q = 0;
    if (q > kQuartiles - 1) q = kQuartiles - 1;
    out->quartileOf[order[rank]] = static_cast<uint8_t>(q);
  }

  aggregateCells(*out, &out->cells);
  aggregateBuckets(*out, out->quartileOf, &out->quartiles);

  double sumLum = 0.0;
  for (size_t i = 0; i < n; ++i) sumLum += out->lumPlane[i];
  out->globalMeanLum = sumLum / static_cast<double>(n);
  // 色度**局部**方差（去噪判据）：a*/b* 的 16×16 块方差之和
  computeBlockVarPlane({&out->aPlane, &out->bPlane}, {1.0, 1.0}, w, h, &out->chromaVarPlane);
  out->chromaVar = planeMean(out->chromaVarPlane);
  return true;
}

}  // namespace

bool computeRegionStats(const imaging::Image& base, RegionStats* out, std::string* err) {
  const int w = base.width;
  const int h = base.height;
  if (w <= 0 || h <= 0) {
    if (err) *err = "region_stats: image has no pixels";
    return false;
  }
  const size_t need = static_cast<size_t>(w) * h * 3;
  if (base.pixels.size() < need) {
    if (err) *err = "region_stats: pixel buffer too small";
    return false;
  }
  const size_t n = static_cast<size_t>(w) * h;
  std::vector<float> r(n), g(n), b(n);
  for (size_t i = 0; i < n; ++i) {
    r[i] = static_cast<float>(base.pixels[i * 3 + 0]);
    g[i] = static_cast<float>(base.pixels[i * 3 + 1]);
    b[i] = static_cast<float>(base.pixels[i * 3 + 2]);
  }
  return computeFromPlanes(r, g, b, w, h, out, err);
}

bool computeRegionStatsBuf(const imaging::ImageBuf& base, RegionStats* out, std::string* err) {
  if (!base.valid()) {
    if (err) *err = "region_stats: invalid ImageBuf";
    return false;
  }
  const size_t n = base.pixelCount();
  // code/65535 视作显示值（S2 域约定），再换算到 8bit 尺度；float 工作链同样适用
  constexpr double kScale = 255.0 / 65535.0;
  std::vector<float> r(n), g(n), b(n);
  for (size_t i = 0; i < n; ++i) {
    r[i] = static_cast<float>(imaging::codeAt(base, i * 3 + 0) * kScale);
    g[i] = static_cast<float>(imaging::codeAt(base, i * 3 + 1) * kScale);
    b[i] = static_cast<float>(imaging::codeAt(base, i * 3 + 2) * kScale);
  }
  return computeFromPlanes(r, g, b, base.w, base.h, out, err);
}

bool compareRegionStats(const RegionStats& base, const RegionStats& cur, LeafDelta* out) {
  if (!out) return false;
  if (base.w != cur.w || base.h != cur.h || base.w <= 0 || base.h <= 0) return false;
  const size_t n = static_cast<size_t>(base.w) * base.h;
  if (base.lumPlane.size() != n || cur.lumPlane.size() != n) return false;
  if (base.cells.size() != static_cast<size_t>(kCellCount) ||
      cur.cells.size() != static_cast<size_t>(kCellCount)) {
    return false;
  }
  if (base.quartileOf.size() != n) return false;

  LeafDelta d;
  // ---- 网格差分 ----
  for (int i = 0; i < kCellCount; ++i) {
    const Cell& b = base.cells[i];
    const Cell& c = cur.cells[i];
    const double dLum = c.meanLum - b.meanLum;
    const double dA = c.meanA - b.meanA;
    const double dB = c.meanB - b.meanB;
    const double absLum = std::fabs(dLum);
    if (absLum > d.maxCellLumDelta || d.worstCell < 0) {
      d.maxCellLumDelta = absLum;
      d.worstCell = i;
    }
    // 色度：以格均值的色度模长差（带符号）表征方向，以欧氏差表征幅度
    const double cb = std::sqrt(b.meanA * b.meanA + b.meanB * b.meanB);
    const double cc = std::sqrt(c.meanA * c.meanA + c.meanB * c.meanB);
    const double dC = cc - cb;
    if (std::fabs(dC) > std::fabs(d.maxCellChromaDelta)) d.maxCellChromaDelta = dC;
    const double euclid = std::sqrt(dA * dA + dB * dB);
    if (euclid > d.maxCellChromaDeltaAbs) d.maxCellChromaDeltaAbs = euclid;
  }
  // ---- 整幅方差 / 边缘能量（相对变化；口径 = 「块方差均值」「Sobel 幅值均值」）----
  const double varBase = planeMean(base.varPlane);
  const double varCur = planeMean(cur.varPlane);
  d.varRelChange = varBase > 1e-9 ? (varCur - varBase) / varBase : 0.0;
  const double edgeBase = planeMean(base.edgePlane);
  const double edgeCur = planeMean(cur.edgePlane);
  d.edgeRelChange = edgeBase > 1e-9 ? (edgeCur - edgeBase) / edgeBase : 0.0;
  // ---- 四分位桶（用基线的桶归属重新统计 cur：同一批像素）----
  std::vector<Bucket> curBuckets;
  aggregateBuckets(cur, base.quartileOf, &curBuckets);
  for (int q = 0; q < kQuartiles; ++q) {
    const Bucket& b = base.quartiles[static_cast<size_t>(q)];
    const Bucket& c = curBuckets[static_cast<size_t>(q)];
    d.quartileLumDelta[q] = c.meanLum - b.meanLum;
  }
  // ---- 整幅均值 / 逐像素最大差 ----
  d.globalMeanDelta = std::fabs(cur.globalMeanLum - base.globalMeanLum);
  double maxPixel = 0.0;
  for (size_t i = 0; i < n; ++i) {
    const double diff = std::fabs(static_cast<double>(cur.lumPlane[i]) - base.lumPlane[i]);
    if (diff > maxPixel) maxPixel = diff;
  }
  d.globalMaxDelta = maxPixel;
  // 色度局部方差相对变化（用同一「16×16 块」口径，避免大尺度色相结构稀释）
  const double cvarBase = planeMean(base.chromaVarPlane);
  const double cvarCur = planeMean(cur.chromaVarPlane);
  d.chromaVarRelChange = cvarBase > 1e-9 ? (cvarCur - cvarBase) / cvarBase : 0.0;
  *out = d;
  return true;
}

}  // namespace testsupport
}  // namespace lumina
