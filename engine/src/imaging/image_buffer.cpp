// ImageBuf 实现：批量 log16 编解码
#include "imaging/image_buffer.h"

namespace lumina {
namespace imaging {

void toLog16(const float* src, uint16_t* dst, size_t n, int32_t logStep) {
  for (size_t i = 0; i < n; ++i) dst[i] = log16Encode(src[i], logStep);
}

void fromLog16(const uint16_t* src, float* dst, size_t n, int32_t logStep) {
  for (size_t i = 0; i < n; ++i) dst[i] = log16Decode(src[i], logStep);
}

}  // namespace imaging
}  // namespace lumina
