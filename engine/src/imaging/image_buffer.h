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
  std::vector<uint16_t> data;  // w*h*3，交错 RGB，log16 码值（存储/缓存权威格式）
  // T1.5：Plan 求值期间的 float 工作缓冲（与 data 同尺寸，互斥使用）。
  // GPU 预览全链 float32 融合 pass；uint16 逐算子 quantize 会在 S4 hsl 色相旋转 /
  // 257 栅格点曲线上把 ±1 码量化差放大成数十 LSB（export=preview 断言超门槛）。
  // executePlan 开链时从 data 填充，链尾一次性 quantize 回 data——uint16 权威不变。
  std::vector<float> dataF;

  ImageBuf() = default;
  ImageBuf(int width, int height, ColorSpaceTag s = ColorSpaceTag::SceneLinearCamera)
      : w(width), h(height), space(s), data(static_cast<size_t>(width) * height * 3, 0u) {}

  bool floatMode() const { return !dataF.empty(); }
  size_t pixelCount() const { return static_cast<size_t>(w) * h; }
  size_t channelCount() const { return pixelCount() * 3; }
  bool valid() const {
    return w > 0 && h > 0 &&
           (floatMode() ? dataF.size() == channelCount() : data.size() == channelCount());
  }
};

// ---- 码值读写助手（T1.5 float 工作链的唯一读写入口）----
// 读：float 模式读 dataF；否则读 uint16 data。
inline float codeAt(const ImageBuf& b, size_t i) {
  return b.floatMode() ? b.dataF[i] : static_cast<float>(b.data[i]);
}
// 写：float 模式直写（clamp 到码域，不 quantize）；否则 quantize 回 uint16（历史语义）。
inline void setCodeAt(ImageBuf& b, size_t i, double v) {
  if (v < 0.0) v = 0.0;
  if (v > 65535.0) v = 65535.0;
  if (b.floatMode()) {
    b.dataF[i] = static_cast<float>(v);
  } else {
    b.data[i] = static_cast<uint16_t>(v + 0.5);
  }
}

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

// T1.5：float 码值解码（float 工作链用；与 uint16 版同式）
inline float log16DecodeF(float u, int32_t logStep = 2048) {
  return static_cast<float>(std::exp2(static_cast<double>(u) / logStep - 12.0));
}

// 批量转换（算子内部 tile 用 float，跨算子边界一律 quantize 回 log16）
void toLog16(const float* src, uint16_t* dst, size_t n, int32_t logStep = 2048);
void fromLog16(const uint16_t* src, float* dst, size_t n, int32_t logStep = 2048);

}  // namespace imaging
}  // namespace lumina

#endif  // LUMINA_IMAGING_IMAGE_BUFFER_H_
