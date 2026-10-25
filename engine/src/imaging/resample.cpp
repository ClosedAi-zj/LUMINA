#include "imaging/resample.h"

#include <cmath>
#include <vector>

namespace lumina {
namespace imaging {

namespace {

constexpr double kPi = 3.14159265358979323846;

// 可分离单趟 1D 重采样：沿 axis 方向 srcW→dstW（src 是解码后的 float 线性数据）。
// 每 dst 采样：中心 srcC = (d + 0.5) * scale + offset - 0.5，窗口内加权求和。
// dataPtr 布局：交错 RGB，行主序，行距 = width*3。
void resamplePass(const float* src, int srcW, int srcH,
                  int dstW, int dstH, bool horizontal, Kernel k,
                  float* dst) {
  const int inSize = horizontal ? srcW : srcH;
  const int outSize = horizontal ? dstW : dstH;
  // 缩小用 scale<1 时核按缩放比展宽（抗锯齿）；放大时固定核半径
  const double scale = static_cast<double>(inSize) / outSize;
  const double filterScale = scale < 1.0 ? scale : 1.0;
  const double radius = kernelRadius(k) / filterScale;
  const int step = horizontal ? 3 : (static_cast<long long>(srcW) * 3);
  const int rowLen = horizontal ? srcH : srcW;  // 另一维度长度
  // 每输出索引的窗口范围缓存
  struct Window { int lo; int hi; };  // [lo, hi) 采样索引
  std::vector<Window> windows(static_cast<size_t>(outSize));
  std::vector<std::vector<double>> weights(static_cast<size_t>(outSize));
  for (int d = 0; d < outSize; ++d) {
    const double center = (d + 0.5) * scale - 0.5;
    int lo = static_cast<int>(std::floor(center - radius)) + 1;
    int hi = static_cast<int>(std::floor(center + radius)) + 1;
    if (lo < 0) lo = 0;
    if (hi > inSize) hi = inSize;
    windows[d] = {lo, hi};
    weights[d].reserve(static_cast<size_t>(hi - lo));
    double sum = 0.0;
    for (int s = lo; s < hi; ++s) {
      const double wgt = kernelWeight(k, (s - center) * filterScale);
      weights[d].push_back(wgt);
      sum += wgt;
    }
    if (sum != 0.0) {
      for (double& wgt : weights[d]) wgt /= sum;  // 归一化（clamp 边缘窗口）
    }
  }
  const long long total = static_cast<long long>(dstW) * dstH;
  for (long long outIdx = 0; outIdx < total; ++outIdx) {
    // outIdx → (d, other)：horizontal 时 d=列，other=行；反之 d=行，other=列
    int d;
    int other;
    if (horizontal) {
      d = static_cast<int>(outIdx % dstW);
      other = static_cast<int>(outIdx / dstW);
    } else {
      d = static_cast<int>(outIdx / dstW);
      other = static_cast<int>(outIdx % dstW);
    }
    const Window& win = windows[d];
    const float* base = src + (horizontal
        ? static_cast<long long>(other) * srcW * 3
        : static_cast<long long>(other) * 3);
    float acc[3] = {0.f, 0.f, 0.f};
    for (int s = win.lo; s < win.hi; ++s) {
      const float wgt = static_cast<float>(weights[d][s - win.lo]);
      const float* p = base + static_cast<long long>(s) * step;
      acc[0] += wgt * p[0];
      acc[1] += wgt * p[1];
      acc[2] += wgt * p[2];
    }
    float* q = dst + static_cast<long long>(outIdx) * 3;
    q[0] = acc[0];
    q[1] = acc[1];
    q[2] = acc[2];
  }
}

}  // namespace

int kernelRadius(Kernel k) {
  switch (k) {
    case Kernel::Box: return 1;
    case Kernel::CatmullRom: return 2;
    case Kernel::Lanczos3: return 3;
  }
  return 2;
}

double kernelWeight(Kernel k, double t) {
  const double a = std::fabs(t);
  switch (k) {
    case Kernel::Box:
      return a <= 0.5 ? 1.0 : 0.0;
    case Kernel::CatmullRom:
      if (a >= 2.0) return 0.0;
      if (a < 1.0) {
        return 1.5 * a * a * a - 2.5 * a * a + 1.0;
      }
      return -0.5 * a * a * a + 2.5 * a * a - 4.0 * a + 2.0;
    case Kernel::Lanczos3:
      if (a >= 3.0) return 0.0;
      if (a < 1e-8) return 1.0;
      return 3.0 * std::sin(kPi * a) * std::sin(kPi * a / 3.0) /
             (kPi * kPi * a * a);
  }
  return 0.0;
}

bool resampleImage16(const ImageBuf& src, int dstW, int dstH, Kernel k,
                     ImageBuf* out, std::string* err) {
  if (!src.valid() || dstW <= 0 || dstH <= 0 || out == nullptr) {
    if (err) *err = "resampleImage16: invalid src or dst size";
    return false;
  }
  if (dstW == src.w && dstH == src.h) {
    *out = src;  // 尺寸相同：原样拷贝（仍保证正确语义）
    return true;
  }
  // log16 → 线性 float
  std::vector<float> linear(src.channelCount());
  fromLog16(src.data.data(), linear.data(), src.channelCount(), src.logStep);
  // 水平趟：srcW→dstW（行优先 dstW × srcH）
  std::vector<float> mid(static_cast<size_t>(dstW) * src.h * 3);
  resamplePass(linear.data(), src.w, src.h, dstW, src.h, /*horizontal=*/true,
               k, mid.data());
  // 垂直趟：srcH→dstH
  std::vector<float> finalLinear(static_cast<size_t>(dstW) * dstH * 3);
  resamplePass(mid.data(), dstW, src.h, dstW, dstH, /*horizontal=*/false,
               k, finalLinear.data());
  // 重新编码
  out->w = dstW;
  out->h = dstH;
  out->space = src.space;
  out->blackPoint = src.blackPoint;
  out->whitePoint = src.whitePoint;
  out->iccProfileId = src.iccProfileId;
  out->logStep = src.logStep;
  out->data.assign(static_cast<size_t>(dstW) * dstH * 3, 0u);
  toLog16(finalLinear.data(), out->data.data(), out->data.size(), src.logStep);
  return true;
}

}  // namespace imaging
}  // namespace lumina
