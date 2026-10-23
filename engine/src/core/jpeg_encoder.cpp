// JPEG 编码共享层实现
#include "core/jpeg_encoder.h"

#include <cstdint>
#include <vector>

#include "stb_image_write.h"  // 仅用 to_func；STB_IMAGE_WRITE_IMPLEMENTATION 在 imaging.cpp

namespace lumina {
namespace core {
namespace jpeg {
namespace {

// stb 的输出回调：把分块字节追加到 std::string
struct MemorySink {
  std::string* data;
};

void appendChunk(void* context, void* data, int size) {
  auto* sink = static_cast<MemorySink*>(context);
  if (!sink || !sink->data || !data || size <= 0) return;
  sink->data->append(static_cast<const char*>(data), static_cast<size_t>(size));
}

int clampQuality(int quality) {
  if (quality < 1) return 80;  // 与 imaging::encodeJpeg 的缺省口径一致
  if (quality > 100) return 100;
  return quality;
}

bool validImage(const imaging::Image& image, std::string* err) {
  if (image.width <= 0 || image.height <= 0) {
    if (err) *err = "jpeg encode: empty image";
    return false;
  }
  const size_t expect = static_cast<size_t>(image.width) * static_cast<size_t>(image.height) * 3;
  if (image.pixels.size() < expect) {
    if (err) *err = "jpeg encode: pixel buffer too small (expect " + std::to_string(expect) +
                    ", got " + std::to_string(image.pixels.size()) + ")";
    return false;
  }
  return true;
}

}  // namespace

bool encodeToMemory(const imaging::Image& image, int quality, std::string* out, std::string* err) {
  if (!out) {
    if (err) *err = "jpeg encode: null output";
    return false;
  }
  out->clear();
  if (!validImage(image, err)) return false;

  MemorySink sink{out};
  // 预分配上限估算：stb 对 RGB 的典型码率约 0.2~1.0 byte/px，先给 0.5 倍避免多次扩容
  out->reserve(static_cast<size_t>(image.width) * static_cast<size_t>(image.height) / 2 + 4096);
  if (!stbi_write_jpg_to_func(appendChunk, &sink, image.width, image.height, 3,
                              image.pixels.data(), clampQuality(quality))) {
    out->clear();
    if (err) *err = "jpeg encode failed (stb_write_jpg_to_func)";
    return false;
  }
  if (out->empty()) {
    if (err) *err = "jpeg encode failed (empty output)";
    return false;
  }
  return true;
}

bool encodeToFile(const std::string& path, const imaging::Image& image, int quality,
                  std::string* err) {
  if (path.empty()) {
    if (err) *err = "jpeg encode: empty path";
    return false;
  }
  if (!validImage(image, err)) return false;
  // 委托 imaging 层：与 mock 历史实现完全同一条 stbi_write_jpg 路径，字节不变
  return imaging::encodeJpeg(path, image, clampQuality(quality), err);
}

}  // namespace jpeg
}  // namespace core
}  // namespace lumina
