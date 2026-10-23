// 风光专项 · HDR 实现：包围组识别 + 曝光融合 + 线性 DNG / TIFF 落盘 + 产物入库
// 红线：原片零写入（只读 file_path）；解码失败 / 组失效 / 编码失败一律抛异常让 job 变 failed，
//      绝不返回一张空图或空 result 假装成功。
#include "specialty/hdr_service.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

#include "core/jpeg_encoder.h"
#include "core/logger.h"
#include "core/str_util.h"
#include "imaging/imaging.h"

namespace lumina {
namespace specialty {

namespace {

// ============================ 基础工具 ============================

constexpr int kOutputMaxShift = 32;      // autoAlign 平移搜索上限（像素）
constexpr double kGhostLumDelta = 0.15;  // deghost：与中位亮度偏离超过该值即降权
constexpr double kGhostWeight = 0.15;    // 降权系数

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

// "2026-10-22 15:47:01.123" → "20261022-154701"（产物文件名用，避免中文与冒号）
std::string compactStamp(int64_t ms) {
  const std::string text = formatTimestamp(ms);
  std::string digits;
  digits.reserve(17);
  for (char c : text) {
    if (c >= '0' && c <= '9') digits.push_back(c);
  }
  if (digits.size() < 14) return "00000000-000000";
  return digits.substr(0, 8) + "-" + digits.substr(8, 6);
}

// DNG DateTime 格式：YYYY:MM:DD HH:MM:SS
std::string dngDateTime(int64_t ms) {
  std::string text = formatTimestamp(ms > 0 ? ms : nowMs());
  if (text.size() < 19) return "1970:01:01 00:00:00";
  text = text.substr(0, 19);
  text[4] = ':';
  text[7] = ':';
  return text;
}

// ---- 曝光参数解析（photos.shutter / aperture / iso 是展示文本，不是数值列）----
// shutter: "1/125s" "0.5s" "2s" "1/8000"；aperture: "f/2.8" "2.8"
bool parseShutterSeconds(const std::string& text, double* out) {
  std::string s = trim(text);
  while (!s.empty() && (s.back() == 's' || s.back() == 'S' || s.back() == '"' || s.back() == ' ')) {
    s.pop_back();
  }
  if (s.empty()) return false;
  const size_t slash = s.find('/');
  if (slash != std::string::npos) {
    const double num = std::atof(s.substr(0, slash).c_str());
    const double den = std::atof(s.substr(slash + 1).c_str());
    if (num <= 0.0 || den <= 0.0) return false;
    *out = num / den;
    return true;
  }
  const double v = std::atof(s.c_str());
  if (v <= 0.0) return false;
  *out = v;
  return true;
}

bool parseApertureF(const std::string& text, double* out) {
  std::string s = trim(text);
  const size_t f = s.find_first_of("fF");
  if (f != std::string::npos) {
    size_t i = f + 1;
    while (i < s.size() && (s[i] == '/' || s[i] == ' ')) ++i;
    s = s.substr(i);
  }
  const double v = std::atof(s.c_str());
  if (v <= 0.0) return false;
  *out = v;
  return true;
}

// EV100 = log2(N²/t) - log2(ISO/100)：值越大画面越暗（曝光越少）
bool exposureValue(const std::string& shutter, const std::string& aperture, int iso, double* out) {
  double t = 0.0;
  double n = 0.0;
  if (!parseShutterSeconds(shutter, &t)) return false;
  if (!parseApertureF(aperture, &n)) return false;
  const int isoValue = iso > 0 ? iso : 100;
  *out = std::log2((n * n) / t) - std::log2(static_cast<double>(isoValue) / 100.0);
  return true;
}

// 文件名数字尾号（"P0000123.JPG" → 123），用于"无 seq_id 时按文件名连续"判定
bool nameParts(const std::string& fileName, std::string* prefix, int64_t* number) {
  std::string stem = fileName;
  const size_t dot = stem.find_last_of('.');
  if (dot != std::string::npos) stem = stem.substr(0, dot);
  size_t i = stem.size();
  while (i > 0 && stem[i - 1] >= '0' && stem[i - 1] <= '9') --i;
  if (i == stem.size()) return false;
  *prefix = stem.substr(0, i);
  int64_t value = 0;
  for (size_t k = i; k < stem.size(); ++k) {
    value = value * 10 + (stem[k] - '0');
    if (value > 1000000000000LL) return false;
  }
  *number = value;
  return true;
}

// ============================ 灰度金字塔（autoAlign）============================

struct GrayImage {
  int width = 0;
  int height = 0;
  std::vector<uint8_t> pixels;
};

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

// 平均绝对差：ref(x,y) 与 tgt(x+dx,y+dy) 的差异。重叠不足 1/3 视为无效匹配。
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

// 曝光归一化：包围曝光各帧整体亮度差好几档，直接比灰度会把「曝光差」误当成「位移」。
// 所以先做零均值/单位方差拉伸，只保留结构信息（这正是平移搜索需要的）。
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

// 由粗到细的平移搜索：1/8 上 ±(maxShift/8)，逐级细化到全分辨率 ±2，最终钳制到 ±maxShift。
// 各层都先做曝光归一化；若「不位移」的成绩不比最优差 2% 以上，就判定为无需对齐
// （宁可不动，也不要把对齐误差当成抖动打进融合结果）。
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

// ============================ 曝光融合（Mertens 风格）============================

struct FrameView {
  const imaging::Image* image = nullptr;
  int offsetX = 0;
  int offsetY = 0;
};

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

// 权重 = 对比度 × 饱和度 × 曝光良好度（Mertens 三要素），deghost 时对偏离中位亮度的帧降权。
// 亮部帧（过曝）在亮处曝光良好度低、欠曝帧在亮处才接近 0.5，因此自然实现"亮部取欠曝帧"。
void fuseFrames(const std::vector<FrameView>& frames, bool deghost, JobContext* ctx,
                imaging::Image* out, bool* cancelled) {
  const imaging::Image& ref = *frames[0].image;
  out->width = ref.width;
  out->height = ref.height;
  out->pixels.assign(static_cast<size_t>(ref.width) * static_cast<size_t>(ref.height) * 3, 0);

  const size_t n = frames.size();
  std::vector<double> weights(n, 0.0);
  std::vector<double> lums(n, 0.0);
  std::vector<double> reds(n, 0.0);
  std::vector<double> greens(n, 0.0);
  std::vector<double> blues(n, 0.0);

  for (int y = 0; y < ref.height; ++y) {
    if (ctx != nullptr && ctx->isCancelled()) {
      *cancelled = true;
      return;
    }
    uint8_t* dstRow = &out->pixels[static_cast<size_t>(y) * ref.width * 3];
    for (int x = 0; x < ref.width; ++x) {
      for (size_t i = 0; i < n; ++i) {
        double r = 0.0;
        double g = 0.0;
        double b = 0.0;
        sampleRgb(frames[i], x, y, &r, &g, &b);
        reds[i] = r;
        greens[i] = g;
        blues[i] = b;
        const double lum = 0.299 * r + 0.587 * g + 0.114 * b;
        lums[i] = lum;

        // 对比度：四邻域拉普拉斯绝对值（平坦区域权重低 → 纹理区域保留细节）
        const double lap = std::fabs(sampleLum(frames[i], x - 1, y) + sampleLum(frames[i], x + 1, y) +
                                     sampleLum(frames[i], x, y - 1) + sampleLum(frames[i], x, y + 1) -
                                     4.0 * lum);
        // 饱和度：三通道标准差（色彩饱和处优先，避免灰掉）
        const double mean = (r + g + b) / 3.0;
        const double sat = std::sqrt(((r - mean) * (r - mean) + (g - mean) * (g - mean) +
                                      (b - mean) * (b - mean)) /
                                     3.0);
        // 曝光良好度：三通道都接近 0.5 时最高（σ=0.2）
        const double dr = r - 0.5;
        const double dg = g - 0.5;
        const double db = b - 0.5;
        const double well = std::exp(-(dr * dr + dg * dg + db * db) / (2.0 * 0.2 * 0.2));
        weights[i] = (lap + 0.02) * (sat + 0.02) * (well + 1e-4);
      }

      if (deghost && n > 1) {
        std::vector<double> sorted = lums;
        std::sort(sorted.begin(), sorted.end());
        const double median = sorted[n / 2];
        for (size_t i = 0; i < n; ++i) {
          if (std::fabs(lums[i] - median) > kGhostLumDelta) weights[i] *= kGhostWeight;
        }
      }

      double sumW = 0.0;
      double accR = 0.0;
      double accG = 0.0;
      double accB = 0.0;
      for (size_t i = 0; i < n; ++i) {
        sumW += weights[i];
        accR += weights[i] * reds[i];
        accG += weights[i] * greens[i];
        accB += weights[i] * blues[i];
      }
      if (sumW <= 1e-12) {
        // 单帧或权重全为 0（极端像素）：退化为参考帧取值，绝不输出黑点
        sumW = 1.0;
        accR = reds[0];
        accG = greens[0];
        accB = blues[0];
      }
      const size_t di = static_cast<size_t>(x) * 3;
      dstRow[di + 0] = static_cast<uint8_t>(clampDouble(accR / sumW, 0.0, 1.0) * 255.0 + 0.5);
      dstRow[di + 1] = static_cast<uint8_t>(clampDouble(accG / sumW, 0.0, 1.0) * 255.0 + 0.5);
      dstRow[di + 2] = static_cast<uint8_t>(clampDouble(accB / sumW, 0.0, 1.0) * 255.0 + 0.5);
    }
    if (ctx != nullptr && (y % 64) == 0 && ref.height > 0) {
      ctx->progress(45 + static_cast<int>(40LL * (y + 1) / ref.height),
                    "曝光融合 " + std::to_string(y + 1) + "/" + std::to_string(ref.height) + " 行");
    }
  }
}

// ============================ TIFF / DNG 写出 ============================

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

// baseline RGB8 TIFF（未压缩、小端、单条 strip）：与 export 的 TIFF 布局一致，
// 保证同一引擎导出的 tif 与 HDR 合成的 tif 用同一套读取假设。
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

// 线性 RGB DNG：TIFF-EP + DNG 标签（LinearRaw）。内嵌 JPEG 预览（SubIFD）让图库缩略图、
// raw.getProxy 与 LibRaw unpack_thumb 能直接出图。
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
  // 线性 DNG 用 16bit 存（8bit 线性在暗部只剩几级，会明显断层）
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

  const std::string desc = "LUMINA HDR composite - linear RGB DNG";
  const std::string make = "LUMINA";
  const std::string model = "LUMINA HDR Composite";
  const std::string software = "LUMINA 1.0.0";
  const std::string uniqueModel = "LUMINA HDR Composite";

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

  // 值区块（顺序即落盘顺序）
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
  // XYZ(D65) → 线性 sRGB（Bruce Lindbloom 标准矩阵），×10000 转成有理数
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
    // 16bit 线性（小端）写入
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

// ============================ HdrService ============================

HdrService::HdrService(core::Config* config, db::Database* db, db::PhotoDao* photos,
                       raw::RawDecoder* decoder)
    : config_(config), db_(db), photos_(photos), decoder_(decoder) {}

std::string HdrService::compositePath(int64_t groupId, const std::string& extension) const {
  const std::string dir = joinPath(config_ ? config_->cacheDir() : std::string("."), "composite");
  if (!dirExists(dir) && !makeDirs(dir)) return std::string();
  const std::string base = "hdr-" + std::to_string(groupId) + "-" + compactStamp(nowMs());
  std::string path = joinPath(dir, base + "." + extension);
  for (int n = 2; n < 1000 && fileExists(path); ++n) {
    path = joinPath(dir, base + "-" + std::to_string(n) + "." + extension);
  }
  return path;
}

bool HdrService::decodeFrame(const PhotoRecord& record, imaging::Image* out,
                             std::string* err) const {
  if (!decoder_) {
    if (err) *err = "RAW 解码器未就绪";
    return false;
  }
  std::string rawErr;
  if (decoder_->decodeToBitmap(record.filePath, /*half=*/false, out, &rawErr)) return true;
  std::string genericErr;
  if (imaging::decodeImageFile(record.filePath, out, &genericErr)) return true;
  if (err) {
    *err = "解码失败：" + record.filePath + "（RAW: " + rawErr + "；通用: " + genericErr + "）";
  }
  return false;
}

std::vector<BracketGroup> HdrService::detect(const json::JsonValue& params,
                                             const BracketScanOptions& options,
                                             std::string* err) {
  std::vector<BracketGroup> groups;
  if (!db_) {
    if (err) *err = "数据库未就绪";
    return groups;
  }

  // filter 只用来收窄候选集：包围组识别本身必须看真实拍摄时间与曝光，
  // 所以一律从 photos 现算，不信任任何缓存。
  std::vector<int64_t> allowed;
  bool hasFilter = false;
  const json::JsonValue& filterValue = params.get("filter");
  if (filterValue.isObject() && !filterValue.empty()) {
    Filter filter;
    std::string filterErr;
    if (!filterFromJson(filterValue, &filter, &filterErr)) {
      if (err) *err = "params.filter 非法：" + filterErr;
      return groups;
    }
    std::string listErr;
    if (!photos_ || !photos_->listIds(filter, &allowed, &listErr)) {
      if (err) *err = "按 filter 取候选照片失败：" + listErr;
      return groups;
    }
    hasFilter = true;
    std::sort(allowed.begin(), allowed.end());
    if (allowed.empty()) return groups;
  }

  const std::string sql =
      "SELECT id,file_name,seq_id,iso,aperture,shutter,captured_at FROM photos "
      "WHERE deleted=0 AND is_missing=0 AND captured_at>0 "
      "ORDER BY captured_at ASC, id ASC;";
  std::string dbErr;
  auto stmt = db_->prepareCached("specialty.bracket.candidates", sql, &dbErr);
  if (!stmt || !stmt->valid()) {
    if (err) *err = "读取照片列表失败：" + dbErr;
    return groups;
  }

  struct Candidate {
    int64_t id = 0;
    std::string prefix;
    int64_t number = -1;
    int64_t seqId = 0;
    bool hasSeq = false;
    int64_t capturedAt = 0;
    bool hasEv = false;
    double ev = 0.0;
  };
  std::vector<Candidate> candidates;
  while (stmt->step(&dbErr)) {
    const int64_t id = stmt->columnInt64(0);
    if (hasFilter && !std::binary_search(allowed.begin(), allowed.end(), id)) continue;
    Candidate c;
    c.id = id;
    c.hasSeq = !stmt->columnIsNull(2);
    c.seqId = c.hasSeq ? stmt->columnInt64(2) : 0;
    const int iso = stmt->columnInt(3);
    const std::string aperture = stmt->columnText(4);
    const std::string shutter = stmt->columnText(5);
    c.capturedAt = stmt->columnInt64(6);
    double ev = 0.0;
    if (exposureValue(shutter, aperture, iso, &ev)) {
      c.hasEv = true;
      c.ev = ev;
    }
    nameParts(stmt->columnText(1), &c.prefix, &c.number);
    candidates.push_back(c);
  }
  stmt->reset();
  if (!dbErr.empty()) {
    if (err) *err = "遍历照片列表失败：" + dbErr;
    return groups;
  }

  const int64_t maxGapMs = static_cast<int64_t>(options.maxGapSeconds * 1000.0 + 0.5);
  auto canJoin = [&maxGapMs](const Candidate& prev, const Candidate& cur) -> bool {
    const int64_t gap = cur.capturedAt - prev.capturedAt;
    if (gap < 0 || gap > maxGapMs) return false;
    if (prev.hasSeq && cur.hasSeq && prev.seqId == cur.seqId) return true;
    if (prev.number >= 0 && cur.number == prev.number + 1 && prev.prefix == cur.prefix) return true;
    return false;
  };

  // 连拍串 → 按 maxFrames 切片 → 每片算 EV 跨度，达标的才成组
  size_t start = 0;
  while (start < candidates.size()) {
    size_t end = start + 1;
    while (end < candidates.size() && canJoin(candidates[end - 1], candidates[end])) ++end;
    for (size_t chunk = start; chunk < end; chunk += static_cast<size_t>(options.maxFrames)) {
      const size_t chunkEnd = std::min(end, chunk + static_cast<size_t>(options.maxFrames));
      if (chunkEnd - chunk < 2) continue;
      double minEv = 1e18;
      double maxEv = -1e18;
      int evCount = 0;
      int64_t groupId = 0;
      BracketGroup group;
      for (size_t i = chunk; i < chunkEnd; ++i) {
        const Candidate& c = candidates[i];
        group.photoIds.push_back(c.id);
        if (groupId == 0 || c.id < groupId) groupId = c.id;
        if (c.hasEv) {
          minEv = std::min(minEv, c.ev);
          maxEv = std::max(maxEv, c.ev);
          ++evCount;
        }
      }
      if (evCount < 2) continue;
      const double stops = maxEv - minEv;
      if (stops + 1e-9 < options.minStops) continue;
      group.groupId = groupId;
      group.exposureStops = std::round(stops * 10.0) / 10.0;
      groups.push_back(group);
    }
    start = end;
  }
  return groups;
}

json::JsonValue HdrService::detectBracketGroups(const json::JsonValue& params,
                                                rpc::RpcError* err) {
  BracketScanOptions options;
  const json::JsonValue& gap = params.get("maxGapSeconds");
  if (gap.isNumber()) {
    const double v = gap.asDouble(options.maxGapSeconds);
    if (v <= 0.0 || v > 60.0) {
      *err = rpc::RpcError(rpc::kInvalidParams, "params.maxGapSeconds 超出范围 (0, 60]：" +
                                                    std::to_string(v));
      return json::JsonValue::nullValue();
    }
    options.maxGapSeconds = v;
  }
  const json::JsonValue& minStops = params.get("minStops");
  if (minStops.isNumber()) {
    const double v = minStops.asDouble(options.minStops);
    if (v < 0.0 || v > 10.0) {
      *err = rpc::RpcError(rpc::kInvalidParams,
                           "params.minStops 超出范围 [0, 10]：" + std::to_string(v));
      return json::JsonValue::nullValue();
    }
    options.minStops = v;
  }

  std::string scanErr;
  const std::vector<BracketGroup> groups = detect(params, options, &scanErr);
  if (!scanErr.empty()) {
    *err = rpc::RpcError(rpc::kDbError, "识别包围组失败：" + scanErr);
    return json::JsonValue::nullValue();
  }

  json::JsonValue out = json::JsonValue::arrayValue();
  for (const BracketGroup& group : groups) {
    json::JsonValue item = json::JsonValue::objectValue();
    item.setItem("groupId", group.groupId);
    json::JsonValue ids = json::JsonValue::arrayValue();
    for (int64_t id : group.photoIds) ids.addItem(id);
    item.setItem("photoIds", ids);
    item.setItem("exposureStops", group.exposureStops);
    out.addItem(item);
  }
  return out;
}

json::JsonValue HdrService::composite(const json::JsonValue& params, JobManager* jobs,
                                      rpc::RpcError* err) {
  if (!jobs) {
    *err = rpc::RpcError(rpc::kInternalError, "任务管理器未就绪");
    return json::JsonValue::nullValue();
  }
  if (!photos_ || !decoder_ || !config_) {
    *err = rpc::RpcError(rpc::kInternalError, "HDR 服务未就绪（依赖缺失）");
    return json::JsonValue::nullValue();
  }
  const int64_t groupId = params.get("groupId").asInt(0);
  if (groupId <= 0) {
    *err = rpc::RpcError(rpc::kInvalidParams, "params.groupId 缺失或非法：" + std::to_string(groupId));
    return json::JsonValue::nullValue();
  }

  HdrOptions options;
  const json::JsonValue& optionValue = params.get("options");
  if (!optionValue.isNull() && !optionValue.isObject()) {
    *err = rpc::RpcError(rpc::kInvalidParams, "params.options 必须是对象");
    return json::JsonValue::nullValue();
  }
  if (optionValue.isObject()) {
    options.deghost = optionValue.get("deghost").asBool(true);
    options.autoAlign = optionValue.get("autoAlign").asBool(true);
    const std::string format = toLowerAscii(optionValue.get("outputFormat").asString("dng"));
    if (format != "dng" && format != "tiff") {
      *err = rpc::RpcError(rpc::kInvalidParams,
                           "options.outputFormat 仅支持 dng | tiff，收到：" + format);
      return json::JsonValue::nullValue();
    }
    options.outputFormat = format;
  }

  // 组 ID 是组内最小 photoId；重新识别一遍保证"合成的一定是当前库里真实存在的包围组"
  BracketScanOptions scan;
  std::string scanErr;
  const std::vector<BracketGroup> groups = detect(params, scan, &scanErr);
  if (!scanErr.empty()) {
    *err = rpc::RpcError(rpc::kDbError, "识别包围组失败：" + scanErr);
    return json::JsonValue::nullValue();
  }
  std::vector<int64_t> photoIds;
  for (const BracketGroup& group : groups) {
    if (group.groupId == groupId) {
      photoIds = group.photoIds;
      break;
    }
  }
  if (photoIds.size() < 2) {
    *err = rpc::RpcError(rpc::kInvalidParams,
                         "包围组不存在或已失效（groupId=" + std::to_string(groupId) +
                             "），请重新调用 hdr.detectBracketGroups 后再合成");
    return json::JsonValue::nullValue();
  }
  for (int64_t photoId : photoIds) {
    PhotoRecord record;
    std::string dbErr;
    if (!photos_->getById(photoId, &record, &dbErr)) {
      *err = rpc::RpcError(rpc::kFileNotFound,
                           "包围组内照片不存在（photoId=" + std::to_string(photoId) +
                               "）：" + (dbErr.empty() ? std::string("无此记录") : dbErr));
      return json::JsonValue::nullValue();
    }
    if (record.isMissing || !fileExists(record.filePath)) {
      *err = rpc::RpcError(rpc::kFileNotFound,
                           "包围组内照片源文件缺失（photoId=" + std::to_string(photoId) +
                               "）：" + record.filePath);
      return json::JsonValue::nullValue();
    }
  }

  const int64_t jobId = jobs->start("hdr.composite", [this, photoIds, options](JobContext& ctx) {
    runComposite(photoIds, options, ctx);
  });
  json::JsonValue out = json::JsonValue::objectValue();
  out.setItem("jobId", jobId);
  return out;
}

void HdrService::runComposite(const std::vector<int64_t>& photoIds, const HdrOptions& options,
                              JobContext& ctx) {
  if (!photos_ || !decoder_ || !config_) throw std::runtime_error("HDR 服务未就绪（依赖缺失）");
  const int total = static_cast<int>(photoIds.size());
  if (total < 2) throw std::runtime_error("包围组至少需要 2 帧，当前 " + std::to_string(total) + " 帧");

  // ---- 1. 读取记录 + 全分辨率解码 ----
  std::vector<PhotoRecord> records(static_cast<size_t>(total));
  std::vector<double> evs(static_cast<size_t>(total), 0.0);
  std::vector<bool> hasEv(static_cast<size_t>(total), false);
  std::vector<imaging::Image> frames(static_cast<size_t>(total));

  for (int i = 0; i < total; ++i) {
    if (ctx.isCancelled()) return;
    PhotoRecord record;
    std::string dbErr;
    if (!photos_->getById(photoIds[static_cast<size_t>(i)], &record, &dbErr)) {
      throw std::runtime_error("读取照片失败（photoId=" + std::to_string(photoIds[static_cast<size_t>(i)]) +
                               "）：" + (dbErr.empty() ? std::string("无此记录") : dbErr));
    }
    records[static_cast<size_t>(i)] = record;
    double ev = 0.0;
    if (exposureValue(record.shutter, record.aperture, record.iso, &ev)) {
      evs[static_cast<size_t>(i)] = ev;
      hasEv[static_cast<size_t>(i)] = true;
    }
    ctx.progress(2 + 26 * i / std::max(1, total),
                 "解码第 " + std::to_string(i + 1) + "/" + std::to_string(total) + " 帧");
    std::string decodeErr;
    if (!decodeFrame(record, &frames[static_cast<size_t>(i)], &decodeErr)) {
      throw std::runtime_error("第 " + std::to_string(i + 1) + " 帧" + decodeErr);
    }
    if (frames[static_cast<size_t>(i)].width <= 0 || frames[static_cast<size_t>(i)].height <= 0) {
      throw std::runtime_error("第 " + std::to_string(i + 1) + " 帧解码结果为空：" + record.filePath);
    }
  }
  if (ctx.isCancelled()) return;

  // ---- 2. 参考帧 = EV 中位数（避免拿最亮或最暗的极端帧当基准）----
  int refIndex = 0;
  {
    std::vector<int> order(static_cast<size_t>(total));
    for (int i = 0; i < total; ++i) order[static_cast<size_t>(i)] = i;
    std::sort(order.begin(), order.end(), [&evs, &hasEv](int a, int b) {
      const double ea = hasEv[static_cast<size_t>(a)] ? evs[static_cast<size_t>(a)] : 0.0;
      const double eb = hasEv[static_cast<size_t>(b)] ? evs[static_cast<size_t>(b)] : 0.0;
      return ea < eb;
    });
    refIndex = order[static_cast<size_t>(total / 2)];
  }
  const imaging::Image& ref = frames[static_cast<size_t>(refIndex)];

  // 尺寸不一致时统一到参考帧尺寸（真包围曝光不会不一致；不一致说明混入了别的素材，
  // 缩放而不是报错能保住结果，但会记 warning）
  std::string warning;
  for (int i = 0; i < total; ++i) {
    imaging::Image& frame = frames[static_cast<size_t>(i)];
    if (frame.width == ref.width && frame.height == ref.height) continue;
    imaging::Image scaled;
    if (!imaging::resizeImage(frame, &scaled, ref.width, ref.height)) {
      throw std::runtime_error("第 " + std::to_string(i + 1) + " 帧尺寸归一化失败");
    }
    if (warning.empty()) {
      warning = "组内帧尺寸不一致，已统一到参考帧 " + std::to_string(ref.width) + "x" +
                std::to_string(ref.height);
    }
    frame = scaled;
  }
  ctx.progress(30, "解码完成");

  // ---- 3. 平移对齐（autoAlign）----
  std::vector<FrameView> views(static_cast<size_t>(total));
  for (int i = 0; i < total; ++i) {
    views[static_cast<size_t>(i)].image = &frames[static_cast<size_t>(i)];
  }
  if (options.autoAlign) {
    const GrayImage grayRef = toGray(ref);
    int maxShift = 0;
    for (int i = 0; i < total; ++i) {
      if (ctx.isCancelled()) return;
      if (i == refIndex) continue;
      const GrayImage grayTarget = toGray(frames[static_cast<size_t>(i)]);
      int dx = 0;
      int dy = 0;
      estimateShift(grayRef, grayTarget, kOutputMaxShift, &dx, &dy);
      views[static_cast<size_t>(i)].offsetX = dx;
      views[static_cast<size_t>(i)].offsetY = dy;
      maxShift = std::max(maxShift, std::max(dx < 0 ? -dx : dx, dy < 0 ? -dy : dy));
      ctx.progress(30 + 15 * (i + 1) / std::max(1, total),
                   "对齐第 " + std::to_string(i + 1) + "/" + std::to_string(total) + " 帧");
    }
    logInfo("specialty", "hdr job " + std::to_string(ctx.jobId()) + " aligned, maxShift=" +
                             std::to_string(maxShift) + "px");
  }

  // ---- 4. 曝光融合 ----
  ctx.progress(45, "开始曝光融合");
  imaging::Image fused;
  bool cancelled = false;
  fuseFrames(views, options.deghost, &ctx, &fused, &cancelled);
  if (cancelled || ctx.isCancelled()) return;
  if (fused.width <= 0 || fused.height <= 0) throw std::runtime_error("曝光融合产出为空");

  // ---- 5. 落盘 ----
  ctx.progress(86, "写出合成结果");
  int64_t groupId = photoIds.front();
  for (int64_t id : photoIds) groupId = std::min(groupId, id);
  const std::string extension = options.outputFormat == "tiff" ? "tif" : "dng";
  const std::string path = compositePath(groupId, extension);
  if (path.empty()) throw std::runtime_error("无法创建合成输出目录（{home}/cache/composite）");
  std::string encodeErr;
  const bool encoded =
      options.outputFormat == "tiff"
          ? writeRgb8Tiff(path, fused, &encodeErr)
          : writeLinearRgbDng(path, fused, dngDateTime(records[static_cast<size_t>(refIndex)].capturedAt),
                              &encodeErr);
  if (!encoded) {
    removeFile(path);
    throw std::runtime_error("合成结果编码失败：" + (encodeErr.empty() ? path : encodeErr));
  }
  if (fileSize(path) <= 0) {
    removeFile(path);
    throw std::runtime_error("合成结果文件为空：" + path);
  }

  // ---- 6. 产物入索引（图库可见 + 可进修图台）----
  const PhotoRecord& refRecord = records[static_cast<size_t>(refIndex)];
  PhotoRecord product;
  // photos.folder_id / root_id 是开启的外键（PRAGMA foreign_keys=ON），写 0 会直接
  // FOREIGN KEY constraint failed。合成产物沿用参考帧的归属（图库里和原片同组），
  // 参考帧没有归属时（异常数据）才按缓存目录登记一个专用根/文件夹。
  product.folderId = refRecord.folderId;
  product.rootId = refRecord.rootId;
  if (product.folderId <= 0 || product.rootId <= 0) {
    std::string ensureErr;
    const std::string compositeDir = joinPath(config_->cacheDir(), "composite");
    const int64_t rootId = photos_->ensureRoot(compositeDir, "HDR 合成", &ensureErr);
    const int64_t folderId =
        rootId > 0 ? photos_->ensureFolder(rootId, "", "HDR 合成", &ensureErr) : 0;
    if (rootId <= 0 || folderId <= 0) {
      removeFile(path);
      throw std::runtime_error("合成产物入库失败：无法登记归属文件夹（" + ensureErr + "）");
    }
    product.rootId = rootId;
    product.folderId = folderId;
  }
  product.fileName = baseName(path);
  product.filePath = path;
  product.format = options.outputFormat == "tiff" ? "TIFF" : "DNG";
  product.fileSize = fileSize(path);
  product.width = fused.width;
  product.height = fused.height;
  product.capturedAt = refRecord.capturedAt;
  product.importedAt = nowMs();
  product.camera = refRecord.camera;
  product.lens = refRecord.lens;
  product.iso = refRecord.iso;
  product.aperture = refRecord.aperture;
  product.shutter = refRecord.shutter;
  product.focalLength = refRecord.focalLength;
  product.whiteBalance = refRecord.whiteBalance;
  product.location = refRecord.location;
  product.date = refRecord.capturedAt > 0 ? formatMonth(refRecord.capturedAt) : refRecord.date;
  product.sidecarPath = replaceExtension(path, "xmp");
  product.pairKind = "single";
  product.health = "ok";
  product.hasGps = refRecord.hasGps;
  product.gpsLat = refRecord.gpsLat;
  product.gpsLng = refRecord.gpsLng;
  product.gpsAlt = refRecord.gpsAlt;
  product.hasLocationId = refRecord.hasLocationId;
  product.locationId = refRecord.locationId;

  int64_t newPhotoId = 0;
  std::string upsertErr;
  if (!photos_->upsert(product, &newPhotoId, &upsertErr) || newPhotoId <= 0) {
    // 入库失败不能把文件留在缓存里冒充"完成"，也不能静默成功
    removeFile(path);
    throw std::runtime_error("合成产物入库失败：" +
                             (upsertErr.empty() ? std::string("未返回 photoId") : upsertErr));
  }

  double minEv = 1e18;
  double maxEv = -1e18;
  int evCount = 0;
  for (int i = 0; i < total; ++i) {
    if (!hasEv[static_cast<size_t>(i)]) continue;
    minEv = std::min(minEv, evs[static_cast<size_t>(i)]);
    maxEv = std::max(maxEv, evs[static_cast<size_t>(i)]);
    ++evCount;
  }

  json::JsonValue result = json::JsonValue::objectValue();
  result.setItem("photoId", newPhotoId);
  result.setItem("path", path);
  result.setItem("groupId", groupId);
  result.setItem("frames", static_cast<int64_t>(total));
  result.setItem("outputFormat", options.outputFormat);
  result.setItem("deghost", options.deghost);
  result.setItem("autoAlign", options.autoAlign);
  if (evCount >= 2) result.setItem("exposureStops", std::round((maxEv - minEv) * 10.0) / 10.0);
  if (!warning.empty()) result.setItem("warning", warning);
  if (options.outputFormat == "tiff") {
    // 零静默：TIFF 在引擎内没有通用解码器（LibRaw 只吃 RAW/DNG），告知调用方别以为能直接预览
    json::JsonValue note = json::JsonValue::objectValue();
    note.setItem("code", rpc::kUnsupportedFormat);
    note.setItem("message",
                 "TIFF 产物已写入并入库；引擎内预览/修图解码走 LibRaw（仅 RAW/DNG），"
                 "TIFF 需外部解码支持。如需在修图台直接打开，请选用 outputFormat=dng。");
    result.setItem("notice", note);
  }
  ctx.setResult(result);
  logInfo("specialty", "hdr job " + std::to_string(ctx.jobId()) + " done: photoId=" +
                           std::to_string(newPhotoId) + " frames=" + std::to_string(total) +
                           " format=" + options.outputFormat + " path=" + path);
}

}  // namespace specialty
}  // namespace lumina
