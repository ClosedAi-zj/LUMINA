// 文本光栅化实现（Windows GDI）
#include "export/text_raster.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cstring>

#include "core/str_util.h"

namespace lumina {
namespace textraster {

bool hasNonAscii(const std::string& utf8) {
  for (unsigned char c : utf8) {
    if (c >= 0x80) return true;
  }
  return false;
}

bool rasterizeText(const std::string& utf8, int pixelHeight, RasterText* out, std::string* err) {
  if (!out) {
    if (err) *err = "rasterizeText: out 为空";
    return false;
  }
  out->width = 0;
  out->height = 0;
  out->rgba.clear();
  if (utf8.empty() || pixelHeight <= 0) {
    if (err) *err = "rasterizeText: 空文本或非法字高";
    return false;
  }

  const std::wstring text = utf8ToUtf16(utf8);
  if (text.empty()) {
    if (err) *err = "rasterizeText: UTF-8 → UTF-16 转换失败";
    return false;
  }

  // 1) 以 DEFAULT_GUI_FONT 为底取系统 LOGFONT（中文 Windows = 微软雅黑 UI，
  //    完整 CJK 覆盖；英文 Windows 亦有 Segoe UI 等 Unicode 字体），只改字高与抗锯齿。
  HDC screenDc = GetDC(nullptr);
  if (!screenDc) {
    if (err) *err = "GetDC failed";
    return false;
  }
  HDC memDc = CreateCompatibleDC(screenDc);
  if (!memDc) {
    ReleaseDC(nullptr, screenDc);
    if (err) *err = "CreateCompatibleDC failed";
    return false;
  }

  LOGFONTW lf;
  std::memset(&lf, 0, sizeof(lf));
  HFONT guiFont = reinterpret_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
  if (!guiFont || GetObjectW(guiFont, sizeof(lf), &lf) == 0) {
    // 极端兜底：显式指定常见中文字体
    std::memset(&lf, 0, sizeof(lf));
    lf.lfCharSet = DEFAULT_CHARSET;
    lf.lfQuality = ANTIALIASED_QUALITY;
    wcsncpy(lf.lfFaceName, L"Microsoft YaHei", LF_FACESIZE - 1);
  }
  lf.lfHeight = -pixelHeight;  // 逻辑高度（负 = 字符高度）
  lf.lfWeight = FW_NORMAL;
  lf.lfQuality = ANTIALIASED_QUALITY;  // 灰度抗锯齿（CLEARTYPE 是次像素，取覆盖度会偏）
  HFONT font = CreateFontIndirectW(&lf);
  if (!font) {
    DeleteDC(memDc);
    ReleaseDC(nullptr, screenDc);
    if (err) *err = "CreateFontIndirectW failed";
    return false;
  }
  HGDIOBJ oldFont = SelectObject(memDc, font);

  // 2) 测量文本尺寸（不落盘）
  RECT calc = {0, 0, 0, 0};
  const UINT kFormat = DT_CALCRECT | DT_SINGLELINE | DT_NOCLIP;
  int textW = 0;
  int textH = 0;
  if (DrawTextExW(memDc, const_cast<LPWSTR>(text.c_str()), static_cast<int>(text.size()), &calc,
                  kFormat, nullptr) != 0) {
    textW = calc.right - calc.left;
    textH = calc.bottom - calc.top;
  }
  if (textW <= 0 || textH <= 0) {
    // 兜底测量：用 GetTextExtentPoint32W
    SIZE sz = {0, 0};
    if (!GetTextExtentPoint32W(memDc, text.c_str(), static_cast<int>(text.size()), &sz)) {
      SelectObject(memDc, oldFont);
      DeleteObject(font);
      DeleteDC(memDc);
      ReleaseDC(nullptr, screenDc);
      if (err) *err = "文本测量失败";
      return false;
    }
    textW = sz.cx;
    textH = sz.cy;
  }

  // 3) 32 位自顶向下 DIB（红通道 = 覆盖度）
  const int pad = 2;  // 抗锯齿出血
  const int bmpW = textW + pad * 2;
  const int bmpH = textH + pad * 2;
  BITMAPV5HEADER bi;
  std::memset(&bi, 0, sizeof(bi));
  bi.bV5Size = sizeof(bi);
  bi.bV5Width = bmpW;
  bi.bV5Height = -bmpH;  // 负 = 自顶向下
  bi.bV5Planes = 1;
  bi.bV5BitCount = 32;
  bi.bV5Compression = BI_RGB;
  bi.bV5RedMask = 0x00FF0000;
  bi.bV5GreenMask = 0x0000FF00;
  bi.bV5BlueMask = 0x000000FF;
  bi.bV5AlphaMask = 0xFF000000;

  void* bits = nullptr;
  HBITMAP dib = CreateDIBSection(memDc, reinterpret_cast<const BITMAPINFO*>(&bi), DIB_RGB_COLORS,
                                 &bits, nullptr, 0);
  if (!dib || !bits) {
    SelectObject(memDc, oldFont);
    DeleteObject(font);
    DeleteDC(memDc);
    ReleaseDC(nullptr, screenDc);
    if (err) *err = "CreateDIBSection failed";
    return false;
  }
  HGDIOBJ oldBmp = SelectObject(memDc, dib);

  // 黑底白字（之后取红通道做 alpha）
  SetBkColor(memDc, RGB(0, 0, 0));
  SetTextColor(memDc, RGB(255, 255, 255));
  SetBkMode(memDc, OPAQUE);
  RECT draw = {pad, pad, pad + textW, pad + textH};
  DrawTextExW(memDc, const_cast<LPWSTR>(text.c_str()), static_cast<int>(text.size()), &draw,
              DT_SINGLELINE | DT_NOCLIP | DT_TOP | DT_LEFT, nullptr);

  // 4) 提取：RGBA（RGB=白，A=红通道覆盖度）
  out->width = bmpW;
  out->height = bmpH;
  out->rgba.resize(static_cast<size_t>(bmpW) * bmpH * 4);
  const uint8_t* src = static_cast<const uint8_t*>(bits);
  for (size_t i = 0; i < static_cast<size_t>(bmpW) * bmpH; ++i) {
    // BI_RGB 32bpp 小端布局：B,G,R,X
    const uint8_t coverage = src[i * 4 + 2];
    out->rgba[i * 4 + 0] = 255;
    out->rgba[i * 4 + 1] = 255;
    out->rgba[i * 4 + 2] = 255;
    out->rgba[i * 4 + 3] = coverage;
  }

  // 5) 清理
  SelectObject(memDc, oldBmp);
  SelectObject(memDc, oldFont);
  DeleteObject(dib);
  DeleteObject(font);
  DeleteDC(memDc);
  ReleaseDC(nullptr, screenDc);
  return true;
}

}  // namespace textraster
}  // namespace lumina
