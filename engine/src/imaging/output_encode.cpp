#include "imaging/output_encode.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

#include "imaging/image_buffer.h"

namespace lumina {
namespace imaging {

namespace {

// TPDF 三角分布抖动：两次均匀随机取均值 → [-0.5, 0.5] 三角分布，峰 ±0.5 LSB
double tpdfDither() {
  const double u1 = static_cast<double>(std::rand()) / RAND_MAX;
  const double u2 = static_cast<double>(std::rand()) / RAND_MAX;
  return (u1 + u2) * 0.5 - 0.5;
}

}  // namespace

double linearToSrgb(double linear) {
  if (linear <= 0.0) return 0.0;
  if (linear >= 1.0) return 1.0;
  if (linear <= 0.0031308) return linear * 12.92;
  return 1.055 * std::pow(linear, 1.0 / 2.4) - 0.055;
}

double srgbToLinear(double s) {
  if (s <= 0.0) return 0.0;
  if (s >= 1.0) return 1.0;
  if (s <= 0.04045) return s / 12.92;
  return std::pow((s + 0.055) / 1.055, 2.4);
}

bool encode8bit(const ImageBuf& buf, std::vector<uint8_t>* out,
                std::string* err) {
  if (!buf.valid() || out == nullptr) {
    if (err) *err = "encode8bit: invalid buffer";
    return false;
  }
  out->assign(buf.channelCount(), 0u);
  for (size_t i = 0; i < buf.data.size(); ++i) {
    // log16 码值 → 线性（S3 之后 display-referred，[0,1] 语义）
    const double lin = static_cast<double>(log16Decode(buf.data[i], buf.logStep));
    const double enc = linearToSrgb(lin);
    const double d = tpdfDither();
    double q = std::floor(enc * 255.0 + 0.5 + d);
    q = std::max(0.0, std::min(255.0, q));
    (*out)[i] = static_cast<uint8_t>(q);
  }
  return true;
}

bool encode16bit(const ImageBuf& buf, std::vector<uint16_t>* out,
                 std::string* err) {
  if (!buf.valid() || out == nullptr) {
    if (err) *err = "encode16bit: invalid buffer";
    return false;
  }
  *out = buf.data;  // 码值透传：log16 即 16bit 输出格式（往返精确）
  return true;
}

}  // namespace imaging
}  // namespace lumina
