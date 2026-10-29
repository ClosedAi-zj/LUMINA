// 风光专项 · 共享合成辅助实现：灰度金字塔对齐 + 帧采样 + TIFF/DNG 落地 + 景深堆栈融合
// 本文件取代 hdr_service.cpp 里原本文件私有的同名实现，使 HDR 与景深堆栈共用一份。
#include "specialty/compositing_common.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "core/job_manager.h"
#include "core/jpeg_encoder.h"
#include "core/logger.h"
#include "core/str_util.h"
#include "imaging/imaging.h"

namespace lumina {
namespace compositing {

namespace {

int clampInt(int v, int lo, int hi) {
  if (v < lo) return lo;
  if (v > hi) return hi;
  return v;
}

double clampDouble(double v, double lo, double hi) {
  if (v < lo) return lo;
  if (v > hi) return hi;
  return v;
}

void put16(std::string* out, uint16_t v) {
  out->push_back(static_cast<char>(v & 0xFF));
  out->push_back(static_cast<char>((v >> 8) & 0xFF));
}

void put32(std::string* out, uint32_t v) {
  out->push_back(static_cast<char>(v & 0xFF));
  out->push_back(static_cast<char>((v >> 8) & 0xFF));
  out->push_back(static_cast<char>((v >> 16) & 0xFF));
  out->push_back(static_cast<char>((v >> 24) & 0xFF));
}

size_t align2(size_t v) { return (v + 1) & ~static_cast<size_t>(1); }

struct TiffEntry {
  uint16_t tag = 0;
  uint16_t type = 0;  // 1=BYTE 2=ASCII 3=SHORT 4=LONG 5=RATIONAL 10=SRATIONAL
  uint32_t count = 0;
  uint32_t value = 0;  // 内联值，或值区偏移
};

void writeEntry(std::string* out, const TiffEntry& e) {
  put16(out, e.tag);
  put16(out, e.type);
  put32(out, e.count);
  put32(out, e.value);
}

void sortEntries(std::vector<TiffEntry>* entries) {
  std::sort(entries->begin(), entries->end(),
            [](const TiffEntry& a, const TiffEntry& b) { return a.tag < b.tag; });
}

void appendAsciiBlock(std::string* out, const std::string& text) {
  out->append(text);
  out->push_back('\0');
  if (out->size() % 2 != 0) out->push_back('\0');
}

void appendShort3(std::string* out, uint16_t a, uint16_t b, uint16_t c) {
  put16(out, a);
  put16(out, b);
  put16(out, c);
}

void appendRational(std::string* out, uint32_t num, uint32_t den) {
  put32(out, num);
  put32(out, den);
}

void appendSignedRational(std::string* out, int32_t num, int32_t den) {
  put32(out, static_cast<uint32_t>(num));
  put32(out, static_cast<uint32_t>(den));
}

// 线性 DNG 存的是线性光值：sRGB 8bit → 线性 16bit。
// 若直接把 sRGB 值当线性写，LibRaw 解码时再套一次 sRGB 编码，产物会整体提亮
// （实测 60 灰会被读回 135 灰），所以必须在这里做逆编码，保证「写进去 == 读回来」。
const std::vector<uint16_t>& srgbToLinear16Table() {
  static const std::vector<uint16_t> table = [] {
    std::vector<uint16_t> values(256, 0);
    for (int i = 0; i < 256; ++i) {
      const double v = static_cast<double>(i) / 255.0;
      const double linear = v <= 0.04045 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4);
      values[static_cast<size_t>(i)] = static_cast<uint16_t>(linear * 65535.0 + 0.5);
    }
    return values;
  }();
  return table;
}

}  // namespace

// ============================ 灰度金字塔（autoAlign）============================

GrayImage toGray(const imaging::Image& image) {
  GrayImage gray;
  gray.width = image.width;
  gray.height = image.height;
  const size_t count = static_cast<size_t>(image.width) * static_cast<size_t>(image.height);
  gray.pixels.resize(count);
  for (size_t i = 0; i < count; ++i) {
    const uint8_t* px = &image.pixels[i * 3];
    gray.pixels[i] = static_cast<uint8_t>((77 * px[0] + 150 * px[1] + 29 * px[2]) >> 8);
  }
  return gray;
}

GrayImage halfScale(const GrayImage& src) {
  GrayImage dst;
  dst.width = std::max(1, src.width / 2);
  dst.height = std::max(1, src.height / 2);
  dst.pixels.assign(static_cast<size_t>(dst.width) * static_cast<size_t>(dst.height), 0);
  for (int y = 0; y < dst.height; ++y) {
    for (int x = 0; x < dst.width; ++x) {
      int sum = 0;
      int n = 0;
      for (int oy = 0; oy < 2; ++oy) {
        const int sy = y * 2 + oy;
        if (sy >= src.height) continue;
        for (int ox = 0; ox < 2; ++ox) {
          const int sx = x * 2 + ox;
          if (sx >= src.width) continue;
          sum += src.pixels[static_cast<size_t>(sy) * src.width + sx];
          ++n;
        }
      }
      dst.pixels[static_cast<size_t>(y) * dst.width + x] =
          static_cast<uint8_t>(n > 0 ? sum / n : 0);
    }
  }
  return dst;
}

double shiftScore(const GrayImage& ref, const GrayImage& tgt, int dx, int dy) {
  double sum = 0.0;
  size_t count = 0;
  for (int y = 0; y < ref.height; ++y) {
    const int ty = y + dy;
    if (ty < 0 || ty >= tgt.height) continue;
    const uint8_t* a = &ref.pixels[static_cast<size_t>(y) * ref.width];
    const uint8_t* b = &tgt.pixels[static_cast<size_t>(ty) * tgt.width];
    for (int x = 0; x < ref.width; ++x) {
      const int tx = x + dx;
      if (tx < 0 || tx >= tgt.width) continue;
      const int d = static_cast<int>(a[x]) - static_cast<int>(b[tx]);
      sum += d < 0 ? -d : d;
      ++count;
    }
  }
  if (count < ref.pixels.size() / 3 || count == 0) return 1e9;
  return sum / static_cast<double>(count);
}

GrayImage normalizeContrast(const GrayImage& src) {
  const size_t n = src.pixels.size();
  GrayImage out;
  out.width = src.width;
  out.height = src.height;
  out.pixels.resize(n);
  if (n == 0) return out;
  double sum = 0.0;
  double sum2 = 0.0;
  for (uint8_t v : src.pixels) {
    sum += v;
    sum2 += static_cast<double>(v) * v;
  }
  const double mean = sum / static_cast<double>(n);
  double variance = sum2 / static_cast<double>(n) - mean * mean;
  if (variance < 1.0) variance = 1.0;
  const double sd = std::sqrt(variance);
  for (size_t i = 0; i < n; ++i) {
    const double v = (static_cast<double>(src.pixels[i]) - mean) / sd * 48.0 + 128.0;
    out.pixels[i] = static_cast<uint8_t>(clampDouble(v, 0.0, 255.0) + 0.5);
  }
  return out;
}

void estimateShift(const GrayImage& ref, const GrayImage& target, int maxShift, int* outDx,
                   int* outDy) {
  std::vector<GrayImage> refPyr{normalizeContrast(ref)};
  std::vector<GrayImage> tgtPyr{normalizeContrast(target)};
  for (int level = 0; level < 3; ++level) {
    refPyr.push_back(normalizeContrast(halfScale(refPyr.back())));
    tgtPyr.push_back(normalizeContrast(halfScale(tgtPyr.back())));
  }
  int dx = 0;
  int dy = 0;
  for (int level = 3; level >= 0; --level) {
    const int radius = (level == 3) ? std::max(1, maxShift / 8) : 2;
    const int centerX = (level == 3) ? 0 : dx * 2;
    const int centerY = (level == 3) ? 0 : dy * 2;
    double best = 1e18;
    int bestX = centerX;
    int bestY = centerY;
    for (int oy = -radius; oy <= radius; ++oy) {
      for (int ox = -radius; ox <= radius; ++ox) {
        const double score = shiftScore(refPyr[level], tgtPyr[level], centerX + ox, centerY + oy);
        if (score < best) {
          best = score;
          bestX = centerX + ox;
          bestY = centerY + oy;
        }
      }
    }
    dx = bestX;
    dy = bestY;
  }
  const double scoreZero = shiftScore(refPyr[0], tgtPyr[0], 0, 0);
  const double scoreBest = shiftScore(refPyr[0], tgtPyr[0], dx, dy);
  if (scoreZero <= scoreBest * 1.02) {
    dx = 0;
    dy = 0;
  }
  *outDx = clampInt(dx, -maxShift, maxShift);
  *outDy = clampInt(dy, -maxShift, maxShift);
}

// ============================ 帧采样（融合公共）============================

double sampleLum(const FrameView& view, int x, int y) {
  const imaging::Image& img = *view.image;
  const int sx = clampInt(x + view.offsetX, 0, img.width - 1);
  const int sy = clampInt(y + view.offsetY, 0, img.height - 1);
  const uint8_t* px = &img.pixels[(static_cast<size_t>(sy) * img.width + sx) * 3];
  return (0.299 * px[0] + 0.587 * px[1] + 0.114 * px[2]) / 255.0;
}

void sampleRgb(const FrameView& view, int x, int y, double* r, double* g, double* b) {
  const imaging::Image& img = *view.image;
  const int sx = clampInt(x + view.offsetX, 0, img.width - 1);
  const int sy = clampInt(y + view.offsetY, 0, img.height - 1);
  const uint8_t* px = &img.pixels[(static_cast<size_t>(sy) * img.width + sx) * 3];
  *r = px[0] / 255.0;
  *g = px[1] / 255.0;
  *b = px[2] / 255.0;
}

// ============================ TIFF / DNG 写出 ============================

bool writeRgb8Tiff(const std::string& path, const imaging::Image& image, std::string* err) {
  if (image.width <= 0 || image.height <= 0) {
    if (err) *err = "空图像（宽高非法）";
    return false;
  }
  const size_t need = static_cast<size_t>(image.width) * static_cast<size_t>(image.height) * 3;
  if (image.pixels.size() < need) {
    if (err) *err = "像素缓冲不足";
    return false;
  }
  if (need > 0xFFFFFFFFu) {
    if (err) *err = "图像超过 4GB，超出 baseline TIFF 单条 strip 上限";
    return false;
  }

  const uint32_t w = static_cast<uint32_t>(image.width);
  const uint32_t h = static_cast<uint32_t>(image.height);
  const uint32_t pixelBytes = static_cast<uint32_t>(need);

  std::vector<TiffEntry> ifd;
  ifd.push_back(TiffEntry{256, 4, 1, w});              // ImageWidth
  ifd.push_back(TiffEntry{257, 4, 1, h});              // ImageLength
  ifd.push_back(TiffEntry{258, 3, 3, 0});              // BitsPerSample（值区回填）
  ifd.push_back(TiffEntry{259, 3, 1, 1});              // Compression = none
  ifd.push_back(TiffEntry{262, 3, 1, 2});              // Photometric = RGB
  ifd.push_back(TiffEntry{273, 4, 1, 0});              // StripOffsets（回填）
  ifd.push_back(TiffEntry{277, 3, 1, 3});              // SamplesPerPixel
  ifd.push_back(TiffEntry{278, 4, 1, h});              // RowsPerStrip
  ifd.push_back(TiffEntry{279, 4, 1, pixelBytes});     // StripByteCounts
  ifd.push_back(TiffEntry{282, 5, 1, 0});              // XResolution（值区回填）
  ifd.push_back(TiffEntry{283, 5, 1, 0});              // YResolution（值区回填）
  ifd.push_back(TiffEntry{284, 3, 1, 1});              // PlanarConfiguration = chunky
  ifd.push_back(TiffEntry{296, 3, 1, 2});              // ResolutionUnit = inch
  sortEntries(&ifd);

  const size_t ifdOffset = 8;
  const size_t ifdSize = 2 + ifd.size() * 12 + 4;
  const size_t bitsOffset = ifdOffset + ifdSize;
  const size_t xResOffset = bitsOffset + 6;
  const size_t yResOffset = xResOffset + 8;
  const size_t dataOffset = align2(yResOffset + 8);

  for (TiffEntry& e : ifd) {
    if (e.tag == 258) e.value = static_cast<uint32_t>(bitsOffset);
    if (e.tag == 273) e.value = static_cast<uint32_t>(dataOffset);
    if (e.tag == 282) e.value = static_cast<uint32_t>(xResOffset);
    if (e.tag == 283) e.value = static_cast<uint32_t>(yResOffset);
  }

  std::string out;
  out.reserve(dataOffset + need);
  put16(&out, 0x4949);  // "II" 小端
  put16(&out, 42);
  put32(&out, static_cast<uint32_t>(ifdOffset));
  put16(&out, static_cast<uint16_t>(ifd.size()));
  for (const TiffEntry& e : ifd) writeEntry(&out, e);
  put32(&out, 0);  // 无下一个 IFD
  appendShort3(&out, 8, 8, 8);
  appendRational(&out, 72, 1);
  appendRational(&out, 72, 1);
  out.append(reinterpret_cast<const char*>(image.pixels.data()), need);
  return writeFileBytes(path, out, err);
}

bool writeLinearRgbDng(const std::string& path, const imaging::Image& image,
                       const std::string& dateTime, std::string* err) {
  if (image.width <= 0 || image.height <= 0) {
    if (err) *err = "空图像（宽高非法）";
    return false;
  }
  const size_t samples = static_cast<size_t>(image.width) * static_cast<size_t>(image.height) * 3;
  if (image.pixels.size() < samples) {
    if (err) *err = "像素缓冲不足";
    return false;
  }
  const size_t need = samples * 2;
  if (need > 0xFFFFFFFFu) {
    if (err) *err = "图像超过 4GB，超出 DNG 单条 strip 上限";
    return false;
  }

  // 内嵌预览：长边 1024（够缩略图与快速预览），编码失败则退化为无预览的 DNG（仍可用）
  std::string previewJpeg;
  int previewW = 0;
  int previewH = 0;
  {
    const int longEdge = std::max(image.width, image.height);
    const int target = std::min(1024, longEdge);
    double scale = longEdge > 0 ? static_cast<double>(target) / longEdge : 1.0;
    if (scale > 1.0) scale = 1.0;
    previewW = std::max(1, static_cast<int>(image.width * scale + 0.5));
    previewH = std::max(1, static_cast<int>(image.height * scale + 0.5));
    imaging::Image preview;
    if (imaging::resizeImage(image, &preview, previewW, previewH)) {
      std::string jpegErr;
      if (!core::jpeg::encodeToMemory(preview, 86, &previewJpeg, &jpegErr)) {
        logWarn("specialty", "dng preview jpeg encode failed: " + jpegErr);
        previewJpeg.clear();
      }
    }
  }

  const std::string desc = "LUMINA focus stack composite - linear RGB DNG";
  const std::string make = "LUMINA";
  const std::string model = "LUMINA Focus Stack Composite";
  const std::string software = "LUMINA 1.3.0";
  const std::string uniqueModel = "LUMINA Focus Stack Composite";

  const uint32_t w = static_cast<uint32_t>(image.width);
  const uint32_t h = static_cast<uint32_t>(image.height);
  const uint32_t pixelBytes = static_cast<uint32_t>(need);

  std::vector<TiffEntry> ifd;  // IFD0
  ifd.push_back(TiffEntry{0x0100, 4, 1, w});                          // ImageWidth
  ifd.push_back(TiffEntry{0x0101, 4, 1, h});                          // ImageLength
  ifd.push_back(TiffEntry{0x0102, 3, 3, 0});                          // BitsPerSample
  ifd.push_back(TiffEntry{0x0103, 3, 1, 1});                          // Compression = none
  ifd.push_back(TiffEntry{0x0106, 3, 1, 34892});                      // LinearRaw
  ifd.push_back(TiffEntry{0x010E, 2, static_cast<uint32_t>(desc.size() + 1), 0});
  ifd.push_back(TiffEntry{0x010F, 2, static_cast<uint32_t>(make.size() + 1), 0});
  ifd.push_back(TiffEntry{0x0110, 2, static_cast<uint32_t>(model.size() + 1), 0});
  ifd.push_back(TiffEntry{0x0111, 4, 1, 0});                          // StripOffsets
  ifd.push_back(TiffEntry{0x0112, 3, 1, 1});                          // Orientation
  ifd.push_back(TiffEntry{0x0115, 3, 1, 3});                          // SamplesPerPixel
  ifd.push_back(TiffEntry{0x0117, 4, 1, pixelBytes});                 // StripByteCounts
  ifd.push_back(TiffEntry{0x011C, 3, 1, 1});                          // PlanarConfiguration
  ifd.push_back(TiffEntry{0x0131, 2, static_cast<uint32_t>(software.size() + 1), 0});
  ifd.push_back(TiffEntry{0x0132, 2, static_cast<uint32_t>(dateTime.size() + 1), 0});
  const bool hasPreview = !previewJpeg.empty();
  if (hasPreview) ifd.push_back(TiffEntry{0x014A, 4, 1, 0});           // SubIFDs
  ifd.push_back(TiffEntry{0xC612, 1, 4, 0x00000401u});                 // DNGVersion 1.4.0.0
  ifd.push_back(TiffEntry{0xC613, 1, 4, 0x00000101u});                 // DNGBackwardVersion 1.1.0.0
  ifd.push_back(TiffEntry{0xC614, 2, static_cast<uint32_t>(uniqueModel.size() + 1), 0});
  ifd.push_back(TiffEntry{0xC61A, 3, 1, 0});                           // BlackLevel
  ifd.push_back(TiffEntry{0xC61D, 4, 1, 65535});                        // WhiteLevel（16bit）
  ifd.push_back(TiffEntry{0xC621, 10, 9, 0});                          // ColorMatrix1
  ifd.push_back(TiffEntry{0xC628, 5, 3, 0});                           // AsShotNeutral
  ifd.push_back(TiffEntry{0xC65A, 3, 1, 21});                          // CalibrationIlluminant1 = D65
  sortEntries(&ifd);

  std::string blockDesc;
  appendAsciiBlock(&blockDesc, desc);
  std::string blockMake;
  appendAsciiBlock(&blockMake, make);
  std::string blockModel;
  appendAsciiBlock(&blockModel, model);
  std::string blockSoftware;
  appendAsciiBlock(&blockSoftware, software);
  std::string blockDateTime;
  appendAsciiBlock(&blockDateTime, dateTime);
  std::string blockUnique;
  appendAsciiBlock(&blockUnique, uniqueModel);
  std::string blockBits;
  appendShort3(&blockBits, 16, 16, 16);
  std::string blockMatrix;
  {
    static const double kColorMatrix[9] = {3.2404542, -1.5371385, -0.4985314,
                                           -0.9692660, 1.8760108,  0.0415560,
                                           0.0556434, -0.2040259,  1.0572252};
    for (double v : kColorMatrix) {
      appendSignedRational(&blockMatrix, static_cast<int32_t>(std::lround(v * 10000.0)), 10000);
    }
  }
  std::string blockNeutral;
  appendRational(&blockNeutral, 1, 1);
  appendRational(&blockNeutral, 1, 1);
  appendRational(&blockNeutral, 1, 1);

  const std::string* blocks[] = {&blockDesc,     &blockMake,   &blockModel, &blockSoftware,
                                 &blockDateTime, &blockUnique, &blockBits,  &blockMatrix,
                                 &blockNeutral};
  const size_t blockCount = sizeof(blocks) / sizeof(blocks[0]);

  const size_t ifdOffset = 8;
  const size_t ifdSize = 2 + ifd.size() * 12 + 4;
  std::vector<uint32_t> blockOffsets(blockCount, 0);
  size_t cursor = ifdOffset + ifdSize;
  for (size_t i = 0; i < blockCount; ++i) {
    blockOffsets[i] = static_cast<uint32_t>(cursor);
    cursor = align2(cursor + blocks[i]->size());
  }
  const size_t dataOffset = align2(cursor);
  const size_t previewIfdOffset = align2(dataOffset + need);

  const size_t previewEntryCount = hasPreview ? 11 : 0;
  const size_t previewIfdSize = hasPreview ? (2 + previewEntryCount * 12 + 4) : 0;
  const size_t previewBitsOffset = previewIfdOffset + previewIfdSize;
  const size_t previewJpegOffset = align2(previewBitsOffset + 6);

  for (TiffEntry& e : ifd) {
    if (e.tag == 0x0102) e.value = blockOffsets[6];
    if (e.tag == 0x010E) e.value = blockOffsets[0];
    if (e.tag == 0x010F) e.value = blockOffsets[1];
    if (e.tag == 0x0110) e.value = blockOffsets[2];
    if (e.tag == 0x0111) e.value = static_cast<uint32_t>(dataOffset);
    if (e.tag == 0x0131) e.value = blockOffsets[3];
    if (e.tag == 0x0132) e.value = blockOffsets[4];
    if (e.tag == 0x014A) e.value = static_cast<uint32_t>(previewIfdOffset);
    if (e.tag == 0xC614) e.value = blockOffsets[5];
    if (e.tag == 0xC621) e.value = blockOffsets[7];
    if (e.tag == 0xC628) e.value = blockOffsets[8];
  }

  std::string out;
  out.reserve(previewJpegOffset + previewJpeg.size() + need);
  put16(&out, 0x4949);
  put16(&out, 42);
  put32(&out, static_cast<uint32_t>(ifdOffset));
  put16(&out, static_cast<uint16_t>(ifd.size()));
  for (const TiffEntry& e : ifd) writeEntry(&out, e);
  put32(&out, 0);
  for (size_t i = 0; i < blockCount; ++i) {
    out.append(*blocks[i]);
    if (out.size() % 2 != 0) out.push_back('\0');
  }
  if (out.size() < dataOffset) out.append(dataOffset - out.size(), '\0');
  {
    const std::vector<uint16_t>& table = srgbToLinear16Table();
    out.reserve(out.size() + need);
    for (size_t i = 0; i < samples; ++i) {
      const uint16_t value = table[image.pixels[i]];
      out.push_back(static_cast<char>(value & 0xFF));
      out.push_back(static_cast<char>((value >> 8) & 0xFF));
    }
  }

  if (hasPreview) {
    if (out.size() < previewIfdOffset) out.append(previewIfdOffset - out.size(), '\0');
    std::vector<TiffEntry> sub;
    sub.push_back(TiffEntry{0x00FE, 4, 1, 1});                       // NewSubFileType = preview
    sub.push_back(TiffEntry{0x0100, 4, 1, static_cast<uint32_t>(previewW)});
    sub.push_back(TiffEntry{0x0101, 4, 1, static_cast<uint32_t>(previewH)});
    sub.push_back(TiffEntry{0x0102, 3, 3, static_cast<uint32_t>(previewBitsOffset)});
    sub.push_back(TiffEntry{0x0103, 3, 1, 7});                        // Compression = JPEG
    sub.push_back(TiffEntry{0x0106, 3, 1, 6});                        // Photometric = YCbCr
    sub.push_back(TiffEntry{0x0111, 4, 1, static_cast<uint32_t>(previewJpegOffset)});
    sub.push_back(TiffEntry{0x0115, 3, 1, 3});
    sub.push_back(TiffEntry{0x0117, 4, 1, static_cast<uint32_t>(previewJpeg.size())});
    sub.push_back(TiffEntry{0x0201, 4, 1, static_cast<uint32_t>(previewJpegOffset)});
    sub.push_back(TiffEntry{0x0202, 4, 1, static_cast<uint32_t>(previewJpeg.size())});
    sortEntries(&sub);
    put16(&out, static_cast<uint16_t>(sub.size()));
    for (const TiffEntry& e : sub) writeEntry(&out, e);
    put32(&out, 0);
    appendShort3(&out, 8, 8, 8);
    if (out.size() < previewJpegOffset) out.append(previewJpegOffset - out.size(), '\0');
    out.append(previewJpeg);
  }

  return writeFileAtomic(path, out, err);
}

// ============================ 景深堆栈融合 ============================

void focusStackFrames(const std::vector<FrameView>& frames, double smoothness, JobContext* ctx,
                      imaging::Image* out, bool* cancelled) {
  if (cancelled) *cancelled = false;
  if (frames.empty()) {
    out->width = 0;
    out->height = 0;
    out->pixels.clear();
    return;
  }
  const imaging::Image& ref = *frames[0].image;
  out->width = ref.width;
  out->height = ref.height;
  out->pixels.assign(static_cast<size_t>(ref.width) * static_cast<size_t>(ref.height) * 3, 0);

  const size_t n = frames.size();
  std::vector<double> w(n, 0.0);
  std::vector<double> rs(n, 0.0);
  std::vector<double> gs(n, 0.0);
  std::vector<double> bs(n, 0.0);

  for (int y = 0; y < ref.height; ++y) {
    if (ctx != nullptr && ctx->isCancelled()) {
      if (cancelled) *cancelled = true;
      return;
    }
    uint8_t* dstRow = &out->pixels[static_cast<size_t>(y) * ref.width * 3];
    for (int x = 0; x < ref.width; ++x) {
      double sumW = 0.0;
      for (size_t i = 0; i < n; ++i) {
        double r = 0.0, g = 0.0, b = 0.0;
        sampleRgb(frames[i], x, y, &r, &g, &b);
        rs[i] = r;
        gs[i] = g;
        bs[i] = b;
        const double lum = sampleLum(frames[i], x, y);
        // 焦点测度：四邻域拉普拉斯绝对值（平坦区域权重低 → 清晰纹理区域保留）
        const double lap =
            std::fabs(sampleLum(frames[i], x - 1, y) + sampleLum(frames[i], x + 1, y) +
                     sampleLum(frames[i], x, y - 1) + sampleLum(frames[i], x, y + 1) -
                     4.0 * lum);
        // 权重 = (焦点测度 + 1e-3) ^ smoothness：清晰处权重压倒性高，平滑过渡
        w[i] = std::pow(lap + 1e-3, smoothness);
        sumW += w[i];
      }

      double accR = 0.0, accG = 0.0, accB = 0.0;
      if (sumW <= 1e-12) {
        // 单帧或权重全为 0（极端像素）：退化为参考帧取值，绝不输出黑点
        accR = rs[0];
        accG = gs[0];
        accB = bs[0];
        sumW = 1.0;
      } else {
        for (size_t i = 0; i < n; ++i) {
          accR += w[i] * rs[i];
          accG += w[i] * gs[i];
          accB += w[i] * bs[i];
        }
      }
      const size_t di = static_cast<size_t>(x) * 3;
      dstRow[di + 0] = static_cast<uint8_t>(clampDouble(accR / sumW, 0.0, 1.0) * 255.0 + 0.5);
      dstRow[di + 1] = static_cast<uint8_t>(clampDouble(accG / sumW, 0.0, 1.0) * 255.0 + 0.5);
      dstRow[di + 2] = static_cast<uint8_t>(clampDouble(accB / sumW, 0.0, 1.0) * 255.0 + 0.5);
    }
    if (ctx != nullptr && (y % 64) == 0 && ref.height > 0) {
      ctx->progress(45 + static_cast<int>(40LL * (y + 1) / ref.height),
                    "景深融合 " + std::to_string(y + 1) + "/" + std::to_string(ref.height) + " 行");
    }
  }
}

}  // namespace compositing
}  // namespace lumina
