// JPEG 编码共享层：mock 图库资产 / DNG 内嵌预览 / 导出队列统一走这里（禁止各处复制粘贴）
// 底层仍是 stb_image_write（实现体在 imaging/imaging.cpp，本模块只做统一封装）。
// 字节一致性：encodeToFile 直接委托 imaging::encodeJpeg（stbi_write_jpg 文件写入路径），
// encodeToMemory 走 stbi_write_jpg_to_func —— 两者共用 stb 的同一编码核心与量化表，
// 因此「同一 Image + 同一 quality」产出的 JPEG 字节完全一致。
#ifndef LUMINA_CORE_JPEG_ENCODER_H_
#define LUMINA_CORE_JPEG_ENCODER_H_

#include <string>

#include "imaging/imaging.h"

namespace lumina {
namespace core {
namespace jpeg {

// 编码到内存（内嵌预览、需要字节流的场景）。out 会被整体覆盖。
bool encodeToMemory(const imaging::Image& image, int quality, std::string* out, std::string* err);

// 编码到文件（自动创建父目录）。与 imaging::encodeJpeg 完全等价。
bool encodeToFile(const std::string& path, const imaging::Image& image, int quality,
                  std::string* err);

}  // namespace jpeg
}  // namespace core
}  // namespace lumina

#endif  // LUMINA_CORE_JPEG_ENCODER_H_
