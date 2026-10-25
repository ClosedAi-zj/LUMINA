// ImageBuf：内部权威 16bit 图像载体（架构 §2 决策 D1）
// - 载体：uint16_t 交错 RGB（w*h*3），24MP ≈ 144MB
// - 编码：半对数 log16，scene-linear [2^-12, 2^20) → 16bit 码值，相对精度恒定 ≈0.02%
//   encode(v) = clamp(round((log2(max(v, 2^-12)) + 12) * logStep), 0, 65535)
//   decode(u) = exp2(u / logStep - 12)
// - logStep = 2048（Q1 决议），格式版本化：变更需迁移缓存，禁止运行时改
#ifndef LUMINA_IMAGING_IMAGE_BUFFER_H_
#define LUMINA_IMAGING_IMAGE_BUFFER_H_

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace lumina {
namespace imaging {

// 色彩空间标签随图像传递；跨域转换必须经 plan/color_convert（唯一实现）
enum class ColorSpaceTag {
  SceneLinearCamera,   // S0：传感器线性光（白平衡增益施加于此）
  SceneLinearWorking,  // S1–S2：工作域线性光
  EncodedSRGB,
  EncodedAdobeRGB,
  EncodedDisplayP3,
};

struct ImageBuf {
  int w = 0;
  int h = 0;
  ColorSpaceTag space = ColorSpaceTag::SceneLinearCamera;
  float blackPoint = 0.f;   // 解码时黑电平（供可重现性）
  float whitePoint = 1.f;   // 解码时白电平
  int32_t iccProfileId = -1;  // M4：icc 注册表句柄；-1 = 未绑定
  int32_t logStep = 2048;     // 半对数档宽（格式版本化，禁止运行时改）
  std::vector<uint16_t> data;  // w*h*3，交错 RGB，log16 码值

  ImageBuf() = default;
  ImageBuf(int width, int height, ColorSpaceTag s = ColorSpaceTag::SceneLinearCamera)
      : w(width), h(height), space(s), data(static_cast<size_t>(width) * height * 3, 0u) {}

  size_t pixelCount() const { return static_cast<size_t>(w) * h; }
  size_t channelCount() const { return pixelCount() * 3; }
  bool valid() const { return w > 0 && h > 0 && data.size() == channelCount(); }
};

// log16 编码：scene-linear 浮点 → 16bit 码值。v ≤ 0 归入最低码 0（2^-12 以下无分辨率）。
inline uint16_t log16Encode(float v, int32_t logStep = 2048) {
  const float kFloor = 1.0f / 4096.0f;  // 2^-12
  double x = v < kFloor ? static_cast<double>(kFloor) : static_cast<double>(v);
  double code = (std::log2(x) + 12.0) * static_cast<double>(logStep);
  if (code < 0.0) code = 0.0;
  if (code > 65535.0) code = 65535.0;
  return static_cast<uint16_t>(std::lround(code));
}

// log16 解码：16bit 码值 → scene-linear 浮点（整数位运算等价的浮点实现）
inline float log16Decode(uint16_t u, int32_t logStep = 2048) {
  return static_cast<float>(std::exp2(static_cast<double>(u) / logStep - 12.0));
}

// 批量转换（算子内部 tile 用 float，跨算子边界一律 quantize 回 log16）
void toLog16(const float* src, uint16_t* dst, size_t n, int32_t logStep = 2048);
void fromLog16(const uint16_t* src, float* dst, size_t n, int32_t logStep = 2048);

}  // namespace imaging
}  // namespace lumina

#endif  // LUMINA_IMAGING_IMAGE_BUFFER_H_
