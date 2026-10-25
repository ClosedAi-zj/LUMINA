// T0.7：S7 输出编码（架构 §4 / M0 T0.7）
// - 8bit：线性 → sRGB 传递函数 → TPDF 三角分布抖动（±0.5 LSB）→ 量化
//   （抖动消除等值带状伪影；0.5 常量图输出 127/128 交替、均值 ≈127.5）
// - 16bit：log16 码值原样透传（往返精确无损，无重量化）
// - 输入约束：buf 为 ToneMap 之后 display-referred 线性语义（S3 域分界之后）
#ifndef LUMINA_IMAGING_OUTPUT_ENCODE_H_
#define LUMINA_IMAGING_OUTPUT_ENCODE_H_

#include <cstdint>
#include <string>
#include <vector>

#include "imaging/image_buffer.h"

namespace lumina {
namespace imaging {

// sRGB 传递函数（线性 → 编码，IEC 61966-2-1）
double linearToSrgb(double linear);
// sRGB 逆传递（编码 → 线性），供测试往返验证
double srgbToLinear(double s);

// 8bit 输出：TPDF 抖动量化。out 为交错 RGB（w*h*3）。失败返回 false。
bool encode8bit(const ImageBuf& buf, std::vector<uint8_t>* out,
                std::string* err);

// 16bit 输出：log16 码值透传（无重量化、往返精确）。失败返回 false。
bool encode16bit(const ImageBuf& buf, std::vector<uint16_t>* out,
                 std::string* err);

}  // namespace imaging
}  // namespace lumina

#endif  // LUMINA_IMAGING_OUTPUT_ENCODE_H_
