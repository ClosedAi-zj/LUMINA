// T1.3 共享像素数学实现
#include <cmath>
#include <vector>

#include "plan/ops/ops.h"

namespace lumina {
namespace plan {

void boxBlurPlane(const float* src, float* dst, int w, int h, int radius) {
  if (radius <= 0) {
    for (size_t i = 0; i < static_cast<size_t>(w) * h; ++i) dst[i] = src[i];
    return;
  }
  // 水平 → 垂直两趟滑动窗口
  std::vector<float> tmp(static_cast<size_t>(w) * h);
  for (int y = 0; y < h; ++y) {
    const float* row = src + static_cast<size_t>(y) * w;
    float* trow = &tmp[static_cast<size_t>(y) * w];
    double sum = 0.0;
    int lo = 0;
    int hi = 0;
    const int win = radius * 2 + 1;
    for (int x = 0; x < w; ++x) {
      while (hi < std::min(w, x + radius + 1)) sum += row[hi++];
      while (lo < x - radius) sum -= row[lo++];
      trow[x] = static_cast<float>(sum / std::min(hi - lo, win));
    }
  }
  for (int x = 0; x < w; ++x) {
    double sum = 0.0;
    int lo = 0;
    int hi = 0;
    const int win = radius * 2 + 1;
    for (int y = 0; y < h; ++y) {
      while (hi < std::min(h, y + radius + 1)) sum += tmp[static_cast<size_t>(hi++) * w + x];
      while (lo < y - radius) sum -= tmp[static_cast<size_t>(lo++) * w + x];
      dst[static_cast<size_t>(y) * w + x] = static_cast<float>(sum / std::min(hi - lo, win));
    }
  }
}

namespace {
double hue2rgb(double p, double q, double t) {
  if (t < 0.0) t += 1.0;
  if (t > 1.0) t -= 1.0;
  if (t < 1.0 / 6.0) return p + (q - p) * 6.0 * t;
  if (t < 0.5) return q;
  if (t < 2.0 / 3.0) return p + (q - p) * (2.0 / 3.0 - t) * 6.0;
  return p;
}
}  // namespace

void rgbToHsl(double r, double g, double b, double* h, double* s, double* l) {
  const double mx = std::max(r, std::max(g, b));
  const double mn = std::min(r, std::min(g, b));
  const double d = mx - mn;
  *l = (mx + mn) / 2.0;
  if (d <= 1e-12) {
    *h = 0.0;
    *s = 0.0;
    return;
  }
  *s = *l > 0.5 ? d / (2.0 - mx - mn) : d / (mx + mn);
  if (mx == r) {
    *h = (g - b) / d + (g < b ? 6.0 : 0.0);
  } else if (mx == g) {
    *h = (b - r) / d + 2.0;
  } else {
    *h = (r - g) / d + 4.0;
  }
  *h /= 6.0;
}

void hslToRgb(double h, double s, double l, double* r, double* g, double* b) {
  if (s <= 1e-12) {
    *r = *g = *b = l;
    return;
  }
  const double q = l < 0.5 ? l * (1.0 + s) : l + s - l * s;
  const double p = 2.0 * l - q;
  *r = hue2rgb(p, q, h + 1.0 / 3.0);
  *g = hue2rgb(p, q, h);
  *b = hue2rgb(p, q, h - 1.0 / 3.0);
}

}  // namespace plan
}  // namespace lumina
