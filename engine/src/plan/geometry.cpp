#include "plan/geometry.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include "imaging/image_buffer.h"

namespace lumina {
namespace plan {

using imaging::ImageBuf;

namespace {

constexpr double kEps = 1e-12;

// 3x3 乘法（行主序）
Mat3 mul3(const Mat3& a, const Mat3& b) {
  Mat3 r;
  for (int i = 0; i < 3; ++i) {
    for (int j = 0; j < 3; ++j) {
      double s = 0.0;
      for (int k2 = 0; k2 < 3; ++k2) {
        s += a.m[i * 3 + k2] * b.m[k2 * 3 + j];
      }
      r.m[i * 3 + j] = s;
    }
  }
  return r;
}

// 双线性域插值采样（log16 码值域近似——仅用于窗口内核插值；
// 主重采样质量由独立 resampleImage16 保证，见架构 §10 一致性门槛）
// T1.5：float 工作链读 codeAt、写 float 码值（不 quantize）；uint16 路径保持历史语义。
void sampleBilinear(const ImageBuf& src, double x, double y, double out[3]) {
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

}  // namespace

bool invert3(const Mat3& a, Mat3* inv) {
  const double* m = a.m;
  const double c00 = m[4] * m[8] - m[5] * m[7];
  const double c01 = m[5] * m[6] - m[3] * m[8];
  const double c02 = m[3] * m[7] - m[4] * m[6];
  const double det = m[0] * c00 + m[1] * c01 + m[2] * c02;
  if (std::fabs(det) < kEps) return false;
  const double s = 1.0 / det;
  inv->m[0] = c00 * s;
  inv->m[1] = (m[2] * m[7] - m[1] * m[8]) * s;
  inv->m[2] = (m[1] * m[5] - m[2] * m[4]) * s;
  inv->m[3] = c01 * s;
  inv->m[4] = (m[0] * m[8] - m[2] * m[6]) * s;
  inv->m[5] = (m[2] * m[3] - m[0] * m[5]) * s;
  inv->m[6] = c02 * s;
  inv->m[7] = (m[1] * m[6] - m[0] * m[7]) * s;
  inv->m[8] = (m[0] * m[4] - m[1] * m[3]) * s;
  return true;
}

void mapQuadBounds(const Mat3& fwd, int w, int h,
                   double* minX, double* minY, double* maxX, double* maxY) {
  const double xs[4] = {0.0, static_cast<double>(w), static_cast<double>(w), 0.0};
  const double ys[4] = {0.0, 0.0, static_cast<double>(h), static_cast<double>(h)};
  *minX = *maxX = 0.0;
  *minY = *maxY = 0.0;
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

bool buildForwardHomography(const GeometryParams& p, const ImageBuf& src,
                            Mat3* fwd, int* outW, int* outH, std::string* err) {
  if (!src.valid()) {
    if (err) *err = "buildForwardHomography: invalid src";
    return false;
  }
  if (p.scale <= 0.0) {
    if (err) *err = "buildForwardHomography: scale must be > 0";
    return false;
  }
  // 裁剪区域
  const int cx = std::max(0, p.cropX);
  const int cy = std::max(0, p.cropY);
  const int cw = (p.cropW > 0) ? std::min(p.cropW, src.w - cx) : src.w;
  const int ch = (p.cropH > 0) ? std::min(p.cropH, src.h - cy) : src.h;
  if (cw <= 0 || ch <= 0 || cx >= src.w || cy >= src.h) {
    if (err) *err = "buildForwardHomography: empty crop";
    return false;
  }
  // 单位：源裁剪坐标（0..cw, 0..ch），中心 (cw/2, ch/2)
  const double halfW = cw / 2.0;
  const double halfH = ch / 2.0;
  const double theta = p.angleDeg * 3.14159265358979323846 / 180.0;
  const double cosT = std::cos(theta);
  const double sinT = std::sin(theta);
  // 正向：crop 内 (x,y) → 归一化 → 透视除法 → 平移到中心 → 旋转 → 缩放 → 平移回
  // F = T(cOut) * S(scale) * R(theta) * T(-cCenter) * P(perspective)
  Mat3 persp;  // w' = 1 + pH*nx + pV*ny；nx = x/halfW - 1, ny = y/halfH - 1
  {
    // 展开：w' = 1 + pH*(x/halfW - 1) + pV*(y/halfH - 1)
    //      = (pH/halfW) x + (pV/halfH) y + (1 - pH - pV)
    const double w0 = 1.0 - p.perspectiveH - p.perspectiveV;
    persp.m[0] = p.perspectiveH / halfW;
    persp.m[1] = p.perspectiveV / halfH;
    persp.m[2] = 0.0;
    persp.m[3] = 0.0;
    persp.m[4] = 0.0;
    persp.m[5] = 0.0;
    persp.m[6] = p.perspectiveH / halfW;
    persp.m[7] = p.perspectiveV / halfH;
    persp.m[8] = w0;
    // 注意：这里第 3 列（除法行）与第 1/2 行并行：u = x/w', v = y/w'
    persp.m[0] = 1.0;
    persp.m[1] = 0.0;
    persp.m[2] = 0.0;
    persp.m[3] = 0.0;
    persp.m[4] = 1.0;
    persp.m[5] = 0.0;
  }
  Mat3 toCenter;   // T(-center)
  toCenter.m[2] = -halfW;
  toCenter.m[5] = -halfH;
  Mat3 rot;
  rot.m[0] = cosT; rot.m[1] = -sinT;
  rot.m[3] = sinT; rot.m[4] = cosT;
  Mat3 scl;
  scl.m[0] = p.scale;
  scl.m[4] = p.scale;
  // 输出尺寸：正向映射裁剪四角取包围盒
  Mat3 fwdNoOut = mul3(scl, mul3(rot, mul3(toCenter, persp)));
  double minX, minY, maxX, maxY;
  mapQuadBounds(fwdNoOut, cw, ch, &minX, &minY, &maxX, &maxY);
  const int ow = std::max(1, static_cast<int>(std::ceil(maxX - minX)));
  const int oh = std::max(1, static_cast<int>(std::ceil(maxY - minY)));
  // 把包围盒左上角平移到 (0,0) → 输出像素坐标
  Mat3 shift;
  shift.m[2] = -minX;
  shift.m[5] = -minY;
  *fwd = mul3(shift, fwdNoOut);
  *outW = ow;
  *outH = oh;
  return true;
}

bool applyGeometry(const ImageBuf& src, const GeometryParams& p,
                   imaging::Kernel kernel, ImageBuf* out,
                   uint64_t* sampleCount, std::string* err) {
  (void)kernel;  // 采样核由内部双线性提供；高质量导出经 resampleImage16 独立调用
  Mat3 fwd;
  int outW = 0;
  int outH = 0;
  if (!buildForwardHomography(p, src, &fwd, &outW, &outH, err)) return false;
  Mat3 inv;
  if (!invert3(fwd, &inv)) {
    if (err) *err = "applyGeometry: singular homography";
    return false;
  }
  out->w = outW;
  out->h = outH;
  out->space = src.space;
  out->blackPoint = src.blackPoint;
  out->whitePoint = src.whitePoint;
  out->iccProfileId = src.iccProfileId;
  out->logStep = src.logStep;
  out->data.clear();
  out->dataF.clear();
  const bool floatChain = src.floatMode();
  if (floatChain) {
    out->dataF.assign(static_cast<size_t>(outW) * outH * 3, 0.0f);
  } else {
    out->data.assign(static_cast<size_t>(outW) * outH * 3, 0u);
  }
  uint64_t samples = 0;
  for (int dy = 0; dy < outH; ++dy) {
    for (int dx = 0; dx < outW; ++dx) {
      // dst 像素中心逆映射到 src 坐标
      const double sw = inv.m[6] * dx + inv.m[7] * dy + inv.m[8];
      const double sx = (inv.m[0] * dx + inv.m[1] * dy + inv.m[2]) / sw;
      const double sy = (inv.m[3] * dx + inv.m[4] * dy + inv.m[5]) / sw;
      // 裁剪坐标 → 全图坐标（补回 crop 偏移）
      const double fullX = sx + std::max(0, p.cropX);
      const double fullY = sy + std::max(0, p.cropY);
      double px[3] = {0.0, 0.0, 0.0};
      if (fullX >= 0.0 && fullY >= 0.0 && fullX <= src.w - 1 &&
          fullY <= src.h - 1) {
        sampleBilinear(src, fullX, fullY, px);
      }  // 出界填黑
      const size_t q = (static_cast<size_t>(dy) * outW + dx) * 3;
      if (floatChain) {
        out->dataF[q] = static_cast<float>(px[0]);
        out->dataF[q + 1] = static_cast<float>(px[1]);
        out->dataF[q + 2] = static_cast<float>(px[2]);
      } else {
        out->data[q] = static_cast<uint16_t>(std::lround(px[0]));
        out->data[q + 1] = static_cast<uint16_t>(std::lround(px[1]));
        out->data[q + 2] = static_cast<uint16_t>(std::lround(px[2]));
      }
      ++samples;  // 每 dst 像素恰好一次采样
    }
  }
  if (sampleCount != nullptr) *sampleCount = samples;
  return true;
}

}  // namespace plan
}  // namespace lumina
