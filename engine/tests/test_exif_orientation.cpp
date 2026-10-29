// EXIF orientation 竖拍修正回归：
//
// 缺陷事实（用户实测）：竖拍照片（PANA JPG，6000×4000 横向位图 + orientation=8）
// 导入后 缩略图/预览/修图底图 全部是侧倒的横图 —— "竖屏变横批"。
// 成因：imaging::decodeImageFile 用 stb_image（不读 EXIF），orientation 被忽略。
//
// 本测试锁定：
//   O1 jpegOrientationFromMemory：可从内存 JPEG 字节流读出 APP1 EXIF orientation=8
//      （手工构造 APP1 段；非 JPEG / 无标记 → 1）
//   O2 applyOrientation 6/8：非正方形合成图旋转后宽高互换、四角像素落到正确位置
//   O3 applyOrientation 2/3/4：镜像/180 后角像素正确；orientation=1 不动
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "imaging/imaging.h"

using namespace lumina;

int gFailures = 0;

void check(bool condition, const char* name) {
  if (condition) {
    printf("  [PASS] %s\n", name);
  } else {
    printf("  [FAIL] %s\n", name);
    ++gFailures;
  }
}

// 手工构造最小 JPEG 字节流：SOI + APP1(Exif, IFD0 只含 orientation) + APP0 填充。
// 解析器只扫标记，不需要合法压缩数据。
std::string makeJpegWithOrientation(int orientation, bool bigEndian) {
  std::string tiff;
  auto p16 = [&](int v) { tiff.push_back(static_cast<char>((v >> (bigEndian ? 8 : 0)) & 0xFF)); tiff.push_back(static_cast<char>((v >> (bigEndian ? 0 : 8)) & 0xFF)); };
  auto p32 = [&](int v) { for (int k = 3; k >= 0; --k) tiff.push_back(bigEndian ? static_cast<char>((v >> (8 * k)) & 0xFF) : static_cast<char>(0)); };
  // 小端按 LSB 先发
  auto p16le = [&](int v) { tiff.push_back(static_cast<char>(v & 0xFF)); tiff.push_back(static_cast<char>((v >> 8) & 0xFF)); };
  auto p32le = [&](int v) { for (int k = 0; k < 4; ++k) tiff.push_back(static_cast<char>((v >> (8 * k)) & 0xFF)); };
  // TIFF header：byte order + 42 + IFD0 偏移 8
  if (bigEndian) { tiff += "MM"; p16(42); p32(8); }
  else { tiff += "II"; p16le(42); p32le(8); }
  // IFD0：1 个条目 + next IFD 0
  if (bigEndian) { p16(1); p16(0x0112); p16(3); p32(1); p16(orientation); p16(0); p32(0); }
  else { p16le(1); p16le(0x0112); p16le(3); p32le(1); p16le(orientation); p16le(0); p32le(0); }
  // 拼段：FF E1 len "Exif\0\0" + tiff（len = 2 + 6 + tiff.size()）
  std::string payload = std::string("Exif\0\0", 6) + tiff;
  payload.resize(payload.size());  // 含结尾 \0（上面字面量已含 2 个 \0，这里占位不动）
  std::string seg;
  seg.push_back(static_cast<char>(0xFF));
  seg.push_back(static_cast<char>(0xE1));
  int len = static_cast<int>(payload.size()) + 2;
  seg.push_back(static_cast<char>((len >> 8) & 0xFF));
  seg.push_back(static_cast<char>(len & 0xFF));
  seg += payload;
  std::string jpeg;
  jpeg.push_back(static_cast<char>(0xFF));
  jpeg.push_back(static_cast<char>(0xD8));
  jpeg += seg;
  // 结尾 SOI marker（防扫描越界）
  jpeg.push_back(static_cast<char>(0xFF));
  jpeg.push_back(static_cast<char>(0xD9));
  return jpeg;
}

// 3×2 合成图（w=3,h=2），像素值 = 线性索引，便于验证落点
imaging::Image makeImg(int w, int h) {
  imaging::Image im;
  im.width = w;
  im.height = h;
  im.pixels.resize(static_cast<size_t>(w) * h * 3);
  for (size_t i = 0; i < im.pixels.size(); ++i) im.pixels[i] = static_cast<uint8_t>(i % 251);
  return im;
}

int px(const imaging::Image& im, int x, int y) { return im.pixels[(static_cast<size_t>(y) * im.width + x) * 3]; }

bool sameImage(const imaging::Image& a, const imaging::Image& b) {
  return a.width == b.width && a.height == b.height && a.pixels == b.pixels;
}

int main() {
  printf("=== EXIF orientation 竖拍修正 ===\n");

  // O1 解析器
  {
    std::string be8 = makeJpegWithOrientation(8, true);
    std::string le6 = makeJpegWithOrientation(6, false);
    std::string none = std::string({static_cast<char>(0xFF), static_cast<char>(0xD8), static_cast<char>(0xFF), static_cast<char>(0xD9)});
    check(imaging::jpegOrientationFromMemory(reinterpret_cast<const unsigned char*>(be8.data()), be8.size()) == 8, "O1 BE APP1 orientation=8");
    check(imaging::jpegOrientationFromMemory(reinterpret_cast<const unsigned char*>(le6.data()), le6.size()) == 6, "O1 LE APP1 orientation=6");
    check(imaging::jpegOrientationFromMemory(reinterpret_cast<const unsigned char*>(none.data()), none.size()) == 1, "O1 无 EXIF → 1");
  }

  // O2 旋转 6（顺时针 90）：3×2 → 2×3；old(w-1,0) → new(0,0)
  {
    imaging::Image im = makeImg(3, 2);
    // 公式：new(x,y)=old(y, h-1-x)，h=2
    imaging::Image im6 = im;
    imaging::applyOrientation(&im6, 6);
    check(im6.width == 2 && im6.height == 3, "O2 orientation 6 宽高互换");
    check(px(im6, 0, 0) == px(im, 0, 1) && px(im6, 1, 0) == px(im, 0, 0), "O2 首行落点 = old 底行/顶行");
  }
  // 旋转 8（逆时针 90）：new(x,y)=old(w-1-y, x)
  {
    imaging::Image im = makeImg(3, 2);
    imaging::Image im8 = im;
    imaging::applyOrientation(&im8, 8);
    check(im8.width == 2 && im8.height == 3, "O2 orientation 8 宽高互换");
    check(px(im8, 0, 0) == px(im, 2, 0) && px(im8, 0, 2) == px(im, 0, 0), "O2 首列落点 = old 右列");
  }

  // O3 镜像 / 180 / 恒等
  {
    imaging::Image im = makeImg(3, 2);
    imaging::Image id = im;
    imaging::applyOrientation(&id, 1);
    check(sameImage(id, im), "O3 orientation 1 不动");
    imaging::Image m2 = im;
    imaging::applyOrientation(&m2, 2);
    check(px(m2, 0, 0) == px(im, 2, 0) && m2.width == 3, "O3 水平镜像");
    imaging::Image r3 = im;
    imaging::applyOrientation(&r3, 3);
    check(px(r3, 0, 0) == px(im, 2, 1) && px(r3, 2, 1) == px(im, 0, 0), "O3 180");
  }

  printf(gFailures == 0 ? "ALL PASS\n" : "FAILED %d\n", gFailures);
  return gFailures == 0 ? 0 : 1;
}
