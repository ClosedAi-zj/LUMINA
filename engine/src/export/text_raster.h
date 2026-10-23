// 文本光栅化：Windows GDI + 系统字体（支持中文等全 Unicode）
// 背景：v1.0.0 文字水印原用内置 3x5 点阵（仅 ASCII），中文一律渲染成 '?'。
// 本模块用系统 UI 字体（中文 Windows 即微软雅黑）把文本画成 RGBA 覆盖图，
// 水印合成时按白色 + 不透明度叠加——不引入字体文件/第三方依赖，零新增资产。
#ifndef LUMINA_EXPORT_TEXT_RASTER_H_
#define LUMINA_EXPORT_TEXT_RASTER_H_

#include <cstdint>
#include <string>
#include <vector>

namespace lumina {
namespace textraster {

struct RasterText {
  int width = 0;
  int height = 0;
  std::vector<uint8_t> rgba;  // RGBA8 预乘未做；A = 覆盖度（0..255），RGB 恒白
};

// pixelHeight：期望的字形像素高度（≈  em 盒高度；CJK 字形基本填满 em 盒）。
// 失败返回 false 并置 err（调用方应回退点阵并记 warning，不静默）。
bool rasterizeText(const std::string& utf8, int pixelHeight, RasterText* out, std::string* err);

// UTF-8 字符串是否含非 ASCII 字符（多字节序列首位 >= 0x80）
bool hasNonAscii(const std::string& utf8);

}  // namespace textraster
}  // namespace lumina

#endif  // LUMINA_EXPORT_TEXT_RASTER_H_
