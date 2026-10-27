// T2.10 S1 几何链实现（N4，架构 §4.3 / §3.7.1）。设计注释见 geometry_chain.h。
// 单应展开与 oracle93.buildGeomTransform 逐式一致（double）；aspect 为 T2.10 实装
// 的输出画幅约束（O3），合成方式 = 逆映射第 0 列除以 sx（等价于输出 x 向拉伸）。
#include "plan/geometry_chain.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <vector>

namespace lumina {
namespace plan {

namespace {

constexpr double kPi = 3.14159265358979323846;

struct Mat3 {
  double m[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
};

Mat3 mul3(const Mat3& a, const Mat3& b) {
  Mat3 r;
  for (int i = 0; i < 3; ++i) {
    for (int j = 0; j < 3; ++j) {
      double s = 0.0;
      for (int k = 0; k < 3; ++k) s += a.m[i * 3 + k] * b.m[k * 3 + j];
      r.m[i * 3 + j] = s;
    }
  }
  return r;
}

bool invert3(const Mat3& a, Mat3* inv) {
  const double* m = a.m;
  const double c00 = m[4] * m[8] - m[5] * m[7];
  const double c01 = m[5] * m[6] - m[3] * m[8];
  const double c02 = m[3] * m[7] - m[4] * m[6];
  const double det = m[0] * c00 + m[1] * c01 + m[2] * c02;
  if (std::fabs(det) < 1e-12) return false;
  const double s = 1.0 / det;
  inv->m[0] = c00 * s;
  inv->m[1] = (m[2] * m[7] - m[1] * m[8]) * s;
  inv->m[2] = (m[1] * m[5] - m[2] * m[4]) * s;
  inv->m[3] = c01 * s;
  inv->m[4] = (m[0] * m[8] - m[2] * m[6]) * s;
  inv->m[5] = (m[2] * m[3] - m[0] * m[5]) * s;
  inv->m[6] = c02 * s;
  inv->m[7] = (m[1] * m[6] - m[0] * m[7]) * s;
  inv->m[8] = (m[0] * m[4] - m[2] * m[3]) * s;
  return true;
}

// 正向映射四角包围盒（与 oracle93.mapQuadBounds 同式）
void mapQuadBounds(const Mat3& fwd, double w, double h, double* minX, double* minY,
                   double* maxX, double* maxY) {
  const double xs[4] = {0.0, w, w, 0.0};
  const double ys[4] = {0.0, 0.0, h, h};
  bool first = true;
  for (int i = 0; i < 4; ++i) {
    const double sw = fwd.m[6] * xs[i] + fwd.m[7] * ys[i] + fwd.m[8];
    const double u = (fwd.m[0] * xs[i] + fwd.m[1] * ys[i] + fwd.m[2]) / sw;
    const double v = (fwd.m[3] * xs[i] + fwd.m[4] * ys[i] + fwd.m[5]) / sw;
    if (first) {
      *minX = *maxX = u;
      *minY = *maxY = v;
      first = false;
    } else {
      *minX = std::min(*minX, u);
      *maxX = std::max(*maxX, u);
      *minY = std::min(*minY, v);
      *maxY = std::max(*maxY, v);
    }
  }
}

// log16 码域双线性采样（与 plan/geometry.cpp sampleBilinear / oracle sampleBilinearT
// 同式：clamp 出界、码值直接插值）。float 链读 dataF，uint16 链读 data。
void sampleBilinear(const imaging::ImageBuf& src, double x, double y, double out[3]) {
  const int x0 = static_cast<int>(std::floor(x));
  const int y0 = static_cast<int>(std::floor(y));
  const double fx = x - x0;
  const double fy = y - y0;
  const int x1 = std::min(x0 + 1, src.w - 1);
  const int y1 = std::min(y0 + 1, src.h - 1);
  const int cx0 = std::max(0, std::min(x0, src.w - 1));
  const int cy0 = std::max(0, std::min(y0, src.h - 1));
  for (int c = 0; c < 3; ++c) {
    const double p00 = imaging::codeAt(src, (static_cast<size_t>(cy0) * src.w + cx0) * 3 + c);
    const double p10 = imaging::codeAt(src, (static_cast<size_t>(cy0) * src.w + x1) * 3 + c);
    const double p01 = imaging::codeAt(src, (static_cast<size_t>(y1) * src.w + cx0) * 3 + c);
    const double p11 = imaging::codeAt(src, (static_cast<size_t>(y1) * src.w + x1) * 3 + c);
    const double top = p00 + (p10 - p00) * fx;
    const double bot = p01 + (p11 - p01) * fx;
    out[c] = std::max(0.0, std::min(65535.0, top + (bot - top) * fy));
  }
}

std::atomic<int> g_resampleCount{0};

}  // namespace

GeometryChainParams collectGeometryChainParams(const json::JsonValue& params) {
  GeometryChainParams p;
  p.lensProfileEnable = params.get("optics.lensProfileEnable").asDouble(0.0);
  p.manualDistortion = params.get("optics.manualDistortion").asDouble(0.0);
  p.perspX = params.get("geometry.perspective.x").asDouble(0.0);
  p.perspY = params.get("geometry.perspective.y").asDouble(0.0);
  p.perspH = params.get("geometry.perspective.horizontal").asDouble(0.0);
  p.perspV = params.get("geometry.perspective.vertical").asDouble(0.0);
  p.perspRotate = params.get("geometry.perspective.rotate").asDouble(0.0);
  p.perspScale = params.get("geometry.perspective.scale").asDouble(1.0);
  p.perspAspect = params.get("geometry.perspective.aspect").asDouble(0.0);
  p.hasCrop = params.get("geometry.hasCrop").asBool(false);
  p.cropLeft = params.get("geometry.cropLeft").asDouble(0.0);
  p.cropTop = params.get("geometry.cropTop").asDouble(0.0);
  p.cropRight = params.get("geometry.cropRight").asDouble(100.0);
  p.cropBottom = params.get("geometry.cropBottom").asDouble(100.0);
  p.cropAngle = params.get("geometry.cropAngle").asDouble(0.0);
  return p;
}

void geometryChainResetCounters() { g_resampleCount.store(0); }

int geometryChainResampleCount() { return g_resampleCount.load(); }

bool applyGeometryChain(imaging::ImageBuf* buf, const GeometryChainParams& p,
                        GeometryChainStats* statsOut, std::string* err) {
  if (!buf || !buf->valid()) {
    if (err) *err = "geometry.apply: invalid buffer";
    return false;
  }
  GeometryChainStats stats;
  const double kDistort = p.manualDistortion / 100.0 * 0.3;

  // ---- 子步 1/2/3：镜头 profile / CA / 光学暗角（T2.11 接库前占位 identity）----
  // lensProfileEnable 是选择器叶：无 profile 数据时 identity 不猜测（零静默）。
  // 子步记录：2/3 无叶不记录；1 仅登记选择器激活（当前仍 identity）。
  if (p.lensProfileEnable != 0.0) stats.subSteps.push_back(1);

  // ---- 激活判定（oracle93.buildGeomTransform 同口径 + T2.10 新增 aspect/畸变）----
  const bool cropActive = p.hasCrop || p.cropAngle != 0.0;
  const bool perspActive = p.perspX != 0.0 || p.perspY != 0.0 || p.perspH != 0.0 ||
                           p.perspV != 0.0 || p.perspRotate != 0.0 || p.perspScale != 1.0;
  const bool aspectActive = p.perspAspect != 0.0;
  const bool distortActive = kDistort != 0.0;
  if (!cropActive && !perspActive && !aspectActive && !distortActive) {
    if (statsOut != nullptr) *statsOut = stats;
    return true;  // 恒等：字节不变（aspect=0 → max|Δ|=0 由本判定保证）
  }
  if (perspActive) stats.subSteps.push_back(4);
  if (aspectActive) stats.subSteps.push_back(5);
  if (cropActive) stats.subSteps.push_back(6);
  if (distortActive || p.lensProfileEnable != 0.0) stats.subSteps.insert(stats.subSteps.begin(), 1);

  // ---- 合成正向单应（oracle93.buildGeomTransform 逐式）----
  const int srcW = buf->w;
  const int srcH = buf->h;
  const double cropX = cropActive
                           ? std::trunc(p.cropLeft / 100.0 * srcW)
                           : 0.0;
  const double cropY = cropActive
                           ? std::trunc(p.cropTop / 100.0 * srcH)
                           : 0.0;
  const double cw = cropActive
                        ? std::max(1.0, std::trunc(p.cropRight / 100.0 * srcW) - cropX)
                        : static_cast<double>(srcW);
  const double ch = cropActive
                        ? std::max(1.0, std::trunc(p.cropBottom / 100.0 * srcH) - cropY)
                        : static_cast<double>(srcH);
  const double angleDeg = (cropActive ? p.cropAngle : 0.0) +
                          (perspActive ? p.perspRotate : 0.0);
  const double pH = perspActive ? p.perspH / 100.0 + (p.perspX / 100.0) * 0.1 : 0.0;
  const double pV = perspActive ? p.perspV / 100.0 + (p.perspY / 100.0) * 0.1 : 0.0;
  const double scale = perspActive ? std::max(0.05, p.perspScale) : 1.0;
  const double halfW = cw / 2.0;
  const double halfH = ch / 2.0;
  const double theta = angleDeg * kPi / 180.0;
  const double cosT = std::cos(theta);
  const double sinT = std::sin(theta);
  const double w0 = 1.0 - pH - pV;
  const Mat3 persp = {1, 0, 0, 0, 1, 0, pH / halfW, pV / halfH, w0};
  const Mat3 toCenter = {1, 0, -halfW, 0, 1, -halfH, 0, 0, 1};
  const Mat3 rot = {cosT, -sinT, 0, sinT, cosT, 0, 0, 0, 1};
  const Mat3 scl = {scale, 0, 0, 0, scale, 0, 0, 0, 1};
  const Mat3 fwdNoOut = mul3(scl, mul3(rot, mul3(toCenter, persp)));
  double minX = 0, minY = 0, maxX = 0, maxY = 0;
  mapQuadBounds(fwdNoOut, cw, ch, &minX, &minY, &maxX, &maxY);
  // ---- 子步 5：aspect（O3）——输出画幅强制 w/h = aspect，内容 x 向适配 ----
  // 合成：out2(dx) = out1(dx.x / sx, dx.y)，即逆映射第 0 列除以 sx；宽 ×sx。
  const double realW = maxX - minX;
  const double realH = maxY - minY;
  const double sx = aspectActive ? p.perspAspect * realH / realW : 1.0;
  const int outW = std::max(1, static_cast<int>(std::ceil(realW * sx)));
  const int outH = std::max(1, static_cast<int>(std::ceil(realH)));
  Mat3 fwd = {1, 0, -minX, 0, 1, -minY, 0, 0, 1};
  fwd = mul3(fwd, fwdNoOut);
  Mat3 inv;
  if (!invert3(fwd, &inv)) {
    if (err) *err = "geometry.apply: singular homography";
    return false;
  }
  if (sx != 1.0) {
    inv.m[0] /= sx;
    inv.m[3] /= sx;
    inv.m[6] /= sx;
  }

  // ---- 子步 6：唯一一次重采样（畸变复合在采样坐标上，O4：本循环即全链唯一采样）----
  imaging::ImageBuf out;
  out.w = outW;
  out.h = outH;
  out.space = buf->space;
  out.blackPoint = buf->blackPoint;
  out.whitePoint = buf->whitePoint;
  out.iccProfileId = buf->iccProfileId;
  out.logStep = buf->logStep;
  const bool floatChain = buf->floatMode();
  out.data.clear();
  out.dataF.clear();
  if (floatChain) {
    out.dataF.assign(static_cast<size_t>(outW) * outH * 3, 0.0f);
  } else {
    out.data.assign(static_cast<size_t>(outW) * outH * 3, 0u);
  }
  // 畸变中心 = 中间图（= 源全图，畸变不改尺寸）中心；与 lens.cpp / oracle / GPU 同式
  const double ccx = (srcW - 1) / 2.0;
  const double ccy = (srcH - 1) / 2.0;
  const double maxR = std::sqrt(ccx * ccx + ccy * ccy);
  uint64_t samples = 0;
  for (int dy = 0; dy < outH; ++dy) {
    for (int dx = 0; dx < outW; ++dx) {
      const double sw = inv.m[6] * dx + inv.m[7] * dy + inv.m[8];
      const double mx = (inv.m[0] * dx + inv.m[1] * dy + inv.m[2]) / sw + cropX;
      const double my = (inv.m[3] * dx + inv.m[4] * dy + inv.m[5]) / sw + cropY;
      // 子步 1 复合：畸变逆映射 D（src(D(H_inv(dx)))，畸变先于裁剪/透视的合成形式）
      double fx = mx;
      double fy = my;
      if (distortActive) {
        const double ddx = mx - ccx;
        const double ddy = my - ccy;
        const double r = std::sqrt(ddx * ddx + ddy * ddy) / maxR;
        const double rf = r * (1.0 + kDistort * r * r);
        const double k = rf / std::max(1e-9, r);
        fx = ccx + ddx * k;
        fy = ccy + ddy * k;
      }
      double px[3] = {0.0, 0.0, 0.0};
      if (fx >= 0.0 && fy >= 0.0 && fx <= srcW - 1 && fy <= srcH - 1) {
        sampleBilinear(*buf, fx, fy, px);
      }  // 出界填黑（t=0，与 GPU FRAG_GEO 同判定）
      const size_t q = (static_cast<size_t>(dy) * outW + dx) * 3;
      if (floatChain) {
        out.dataF[q] = static_cast<float>(px[0]);
        out.dataF[q + 1] = static_cast<float>(px[1]);
        out.dataF[q + 2] = static_cast<float>(px[2]);
      } else {
        out.data[q] = static_cast<uint16_t>(std::lround(px[0]));
        out.data[q + 1] = static_cast<uint16_t>(std::lround(px[1]));
        out.data[q + 2] = static_cast<uint16_t>(std::lround(px[2]));
      }
      ++samples;
    }
  }
  stats.samples = samples;
  stats.outW = outW;
  stats.outH = outH;
  stats.resampleCalls = 1;  // 本链恰好一次重采样（O4）
  g_resampleCount.fetch_add(1, std::memory_order_relaxed);
  *buf = std::move(out);
  if (statsOut != nullptr) *statsOut = stats;
  return true;
}

}  // namespace plan
}  // namespace lumina
