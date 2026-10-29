// 通用图像编解码实现（stb_image / stb_image_write）
#include "imaging/imaging.h"

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "core/logger.h"
#include "core/str_util.h"

namespace lumina {
namespace imaging {

bool decodeImageFile(const std::string& path, Image* out, std::string* err) {
  if (!fileExists(path)) {
    if (err) *err = "file not found: " + path;
    return false;
  }
  // 不能用 stbi_load(path)：它内部是窄字符 fopen，Windows 下 UTF-8 中文路径必然
  // 打不开（实测：导入中文名照片后缩略图全部 "decode failed: can't fopen"，
  // 用户看到的是"导进去了但全是灰块"）。改为宽字符 API 读入内存再解码。
  std::string bytes;
  std::string readErr;
  if (!readFile(path, &bytes, &readErr)) {
    if (err) *err = "read failed: " + readErr;
    return false;
  }
  if (bytes.empty()) {
    if (err) *err = "empty file: " + path;
    return false;
  }
  if (bytes.size() > static_cast<size_t>(0x7FFFFFFF)) {
    if (err) *err = "file too large to decode: " + path;
    return false;
  }
  int w = 0, h = 0, n = 0;
  unsigned char* data = stbi_load_from_memory(reinterpret_cast<const stbi_uc*>(bytes.data()),
                                              static_cast<int>(bytes.size()), &w, &h, &n, 3);
  if (!data) {
    if (err) *err = std::string("decode failed: ") + stbi_failure_reason();
    return false;
  }
  out->width = w;
  out->height = h;
  out->pixels.assign(data, data + static_cast<size_t>(w) * h * 3);
  stbi_image_free(data);
  // EXIF orientation 立即转正（stb_image 不读 EXIF；竖拍 JPEG 的存储位图是横向的，
  // 不转正则缩略图/预览/修图底图全部侧倒 —— 用户实测"竖屏变横批"的根因）
  const int orientation = jpegOrientationFromMemory(reinterpret_cast<const unsigned char*>(bytes.data()), bytes.size());
  if (orientation != 1) applyOrientation(out, orientation);
  return true;
}

// ---- EXIF orientation（竖拍修正） ----

int jpegOrientationFromMemory(const unsigned char* data, size_t size) {
  // 最小 JPEG 标记扫描：SOI → APP1("Exif\0\0") → TIFF header → IFD0 tag 0x0112。
  // 只读头部不解码，开销可忽略。
  if (size < 12 || data[0] != 0xFF || data[1] != 0xD8) return 1;
  size_t i = 2;
  while (i + 4 < size) {
    if (data[i] != 0xFF) { ++i; continue; }
    const unsigned char marker = data[i + 1];
    if (marker == 0xE1 && i + 10 < size && std::memcmp(data + i + 4, "Exif\0\0", 6) == 0) {
      const size_t tiff = i + 10;
      if (tiff + 8 > size) return 1;
      const bool big = data[tiff] == 'M' && data[tiff + 1] == 'M';
      auto rd16 = [&](size_t off) -> uint16_t {
        if (off + 2 > size) return 0;
        return big ? static_cast<uint16_t>((data[off] << 8) | data[off + 1])
                   : static_cast<uint16_t>((data[off + 1] << 8) | data[off]);
      };
      auto rd32 = [&](size_t off) -> uint32_t {
        if (off + 4 > size) return 0;
        return big ? ((static_cast<uint32_t>(data[off]) << 24) | (static_cast<uint32_t>(data[off + 1]) << 16) |
                      (static_cast<uint32_t>(data[off + 2]) << 8) | data[off + 3])
                   : ((static_cast<uint32_t>(data[off + 3]) << 24) | (static_cast<uint32_t>(data[off + 2]) << 16) |
                      (static_cast<uint32_t>(data[off + 1]) << 8) | data[off]);
      };
      const uint32_t ifd = rd32(tiff + 4);
      if (ifd == 0 || ifd + 2 > size - tiff) return 1;
      const uint16_t n = rd16(tiff + ifd);
      for (uint16_t k = 0; k < n; ++k) {
        const size_t e = tiff + ifd + 2 + static_cast<size_t>(k) * 12;
        if (e + 12 > size) return 1;
        if (rd16(e) == 0x0112) {
          const uint16_t v = rd16(e + 8);
          return (v >= 1 && v <= 8) ? v : 1;
        }
      }
      return 1;
    }
    if (marker == 0xD8 || marker == 0x01 || (marker >= 0xD0 && marker <= 0xD7)) { i += 2; continue; }
    if (i + 4 > size) return 1;
    const uint16_t len = static_cast<uint16_t>((data[i + 2] << 8) | data[i + 3]);
    if (len < 2) return 1;
    i += 2 + len;
  }
  return 1;
}

void applyOrientation(Image* img, int orientation) {
  if (!img || img->width <= 0 || img->height <= 0 || orientation == 1) return;
  const int w = img->width;
  const int h = img->height;
  const std::vector<uint8_t>& src = img->pixels;
  auto at = [&](int x, int y, int c) -> uint8_t { return src[(static_cast<size_t>(y) * w + x) * 3 + c]; };
  std::vector<uint8_t> dst;
  switch (orientation) {
    case 2:  // 水平镜像
    case 3:  // 180
    case 4:  // 垂直镜像
      dst.resize(src.size());
      for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
          int sx = x, sy = y;
          if (orientation == 2) sx = w - 1 - x;
          else if (orientation == 3) { sx = w - 1 - x; sy = h - 1 - y; }
          else sy = h - 1 - y;
          for (int c = 0; c < 3; ++c) dst[(static_cast<size_t>(y) * w + x) * 3 + c] = at(sx, sy, c);
        }
      }
      break;
    case 5:    // 转置
    case 6:    // 顺时针 90
    case 7:    // 反转置
    case 8: {  // 逆时针 90
      dst.resize(src.size());
      const int W = h, H = w;
      for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
          int sx, sy;
          if (orientation == 5) { sx = y; sy = x; }
          else if (orientation == 6) { sx = y; sy = h - 1 - x; }
          else if (orientation == 7) { sx = w - 1 - y; sy = h - 1 - x; }
          else { sx = w - 1 - y; sy = x; }
          for (int c = 0; c < 3; ++c) dst[(static_cast<size_t>(y) * W + x) * 3 + c] = at(sx, sy, c);
        }
      }
      img->width = W;
      img->height = H;
      break;
    }
    default:
      return;  // 未知值：保持原样（不猜测）
  }
  img->pixels = std::move(dst);
}

bool resizeImage(const Image& src, Image* dst, int targetWidth, int targetHeight) {
  if (src.width <= 0 || src.height <= 0) return false;
  if (targetWidth <= 0) targetWidth = 1;
  if (targetHeight <= 0) targetHeight = 1;
  dst->width = targetWidth;
  dst->height = targetHeight;
  dst->pixels.assign(static_cast<size_t>(targetWidth) * targetHeight * 3, 0);
  const double sx = static_cast<double>(src.width) / targetWidth;
  const double sy = static_cast<double>(src.height) / targetHeight;
  for (int y = 0; y < targetHeight; ++y) {
    int y0 = static_cast<int>(y * sy);
    int y1 = static_cast<int>((y + 1) * sy);
    if (y1 <= y0) y1 = y0 + 1;
    if (y1 > src.height) y1 = src.height;
    for (int x = 0; x < targetWidth; ++x) {
      int x0 = static_cast<int>(x * sx);
      int x1 = static_cast<int>((x + 1) * sx);
      if (x1 <= x0) x1 = x0 + 1;
      if (x1 > src.width) x1 = src.width;
      double r = 0, g = 0, b = 0;
      size_t count = 0;
      for (int yy = y0; yy < y1; ++yy) {
        const uint8_t* row = &src.pixels[static_cast<size_t>(yy) * src.width * 3];
        for (int xx = x0; xx < x1; ++xx) {
          r += row[static_cast<size_t>(xx) * 3 + 0];
          g += row[static_cast<size_t>(xx) * 3 + 1];
          b += row[static_cast<size_t>(xx) * 3 + 2];
          ++count;
        }
      }
      if (count == 0) count = 1;
      size_t di = static_cast<size_t>(y) * targetWidth * 3 + static_cast<size_t>(x) * 3;
      dst->pixels[di + 0] = static_cast<uint8_t>(r / count + 0.5);
      dst->pixels[di + 1] = static_cast<uint8_t>(g / count + 0.5);
      dst->pixels[di + 2] = static_cast<uint8_t>(b / count + 0.5);
    }
  }
  return true;
}

namespace {

// stb 的「写回调」收集器：把编码结果攒进内存，再由 writeFileBytes 走
// UTF-8→Wide 的宽字符 API 落盘。
// 起因与解码侧同源：stbi_write_* 的 path 版本内部是窄字符 fopen，Windows 下
// 任何非 ASCII 路径（中文目录/文件名）都写不出去 —— 导出到「D:/图片/」必然失败。
void collectBytes(void* context, void* data, int size) {
  if (!context || !data || size <= 0) return;
  static_cast<std::string*>(context)->append(static_cast<const char*>(data),
                                             static_cast<size_t>(size));
}

}  // namespace

bool encodeJpeg(const std::string& path, const Image& image, int quality, std::string* err) {
  if (quality < 1) quality = 80;
  if (quality > 100) quality = 100;
  std::string parent = parentPath(path);
  if (!parent.empty() && !dirExists(parent) && !makeDirs(parent)) {
    if (err) *err = "mkdir failed: " + parent;
    return false;
  }
  std::string buffer;
  if (!stbi_write_jpg_to_func(&collectBytes, &buffer, image.width, image.height, 3,
                              image.pixels.data(), quality)) {
    if (err) *err = "jpeg encode failed: " + path;
    return false;
  }
  std::string writeErr;
  if (!writeFileBytes(path, buffer, &writeErr)) {
    if (err) *err = "jpeg write failed: " + path + " (" + writeErr + ")";
    return false;
  }
  return true;
}

bool encodePng(const std::string& path, const Image& image, std::string* err) {
  std::string parent = parentPath(path);
  if (!parent.empty() && !dirExists(parent) && !makeDirs(parent)) {
    if (err) *err = "mkdir failed: " + parent;
    return false;
  }
  std::string buffer;
  if (!stbi_write_png_to_func(&collectBytes, &buffer, image.width, image.height, 3,
                              image.pixels.data(), image.width * 3)) {
    if (err) *err = "png encode failed: " + path;
    return false;
  }
  std::string writeErr;
  if (!writeFileBytes(path, buffer, &writeErr)) {
    if (err) *err = "png write failed: " + path + " (" + writeErr + ")";
    return false;
  }
  return true;
}

void computeHistogram(const Image& image, int64_t r[256], int64_t g[256], int64_t b[256],
                     int64_t* luma) {
  std::memset(r, 0, sizeof(int64_t) * 256);
  std::memset(g, 0, sizeof(int64_t) * 256);
  std::memset(b, 0, sizeof(int64_t) * 256);
  if (luma) std::memset(luma, 0, sizeof(int64_t) * 256);
  const uint8_t* p = image.pixels.data();
  size_t n = static_cast<size_t>(image.width) * image.height;
  for (size_t i = 0; i < n; ++i) {
    const uint8_t R = p[0], G = p[1], B = p[2];
    ++r[R];
    ++g[G];
    ++b[B];
    if (luma) {
      // Rec.709 亮度（与 timelapse_service.cpp 的 meanLuma 一致），落在 0..255 → 合法桶下标
      const uint8_t lumaIdx = static_cast<uint8_t>(0.2126 * R + 0.7152 * G + 0.0722 * B);
      ++luma[lumaIdx];
    }
    p += 3;
  }
}

void applyToneApprox(Image* image, const ToneAdjust& adjust) {
  if (image->pixels.empty()) return;
  double gain = std::pow(2.0, adjust.exposure);
  double contrast = 1.0 + adjust.contrast / 100.0;
  double sat = 1.0 + adjust.saturation / 100.0;
  for (uint8_t& v : image->pixels) {
    double d = v * gain;
    d = (d - 128.0) * contrast + 128.0;
    if (d < 0.0) d = 0.0;
    if (d > 255.0) d = 255.0;
    v = static_cast<uint8_t>(d + 0.5);
  }
  (void)sat;
  // 饱和度近似：按亮度比例混合
  if (adjust.saturation != 0.0) {
    for (size_t i = 0; i < image->pixels.size(); i += 3) {
      double lum = 0.2126 * image->pixels[i] + 0.7152 * image->pixels[i + 1] +
                   0.0722 * image->pixels[i + 2];
      for (int c = 0; c < 3; ++c) {
        double d = lum + (image->pixels[i + c] - lum) * sat;
        if (d < 0.0) d = 0.0;
        if (d > 255.0) d = 255.0;
        image->pixels[i + c] = static_cast<uint8_t>(d + 0.5);
      }
    }
  }
}

// ---- HSL 8 色带近似（WP-A3） ----
// 色带中心角（HSL 色相环，度）：与协议 hsl 键顺序一致
static const double kBandCenterHue[8] = {0.0, 30.0, 60.0, 120.0, 180.0, 240.0, 280.0, 320.0};

int HslBands::bandIndex(const std::string& name) {
  static const char* kNames[8] = {"red", "orange", "yellow", "green",
                                  "aqua", "blue", "purple", "magenta"};
  for (int i = 0; i < 8; ++i) {
    if (name == kNames[i]) return i;
  }
  return -1;
}

namespace {

// 平滑过渡（smoothstep 形：0→0, 0.5→0.5, 1→1），用于 applyToneCurve 的三分区权重
double smooth01(double x) {
  if (x <= 0.0) return 0.0;
  if (x >= 1.0) return 1.0;
  return x * x * (3.0 - 2.0 * x);
}
// 平滑衰减（smoothstep 版，与 plan/ops/ops.h 的 smooth01 同义）：edge0→0，edge1→1（applyColorGrade 分区权重）
double smooth01(double x, double edge0, double edge1) {
  if (edge1 <= edge0) return x >= edge1 ? 1.0 : 0.0;
  double t = (x - edge0) / (edge1 - edge0);
  if (t < 0.0) t = 0.0;
  if (t > 1.0) t = 1.0;
  return t * t * (3.0 - 2.0 * t);
}

// 色相环形距离（0..180 度）
double hueDistance(double a, double b) {
  double d = std::fabs(a - b);
  if (d > 180.0) d = 360.0 - d;
  return d;
}

// 单位口径：hue 输出为角度制 [0,360)，与 plan/ops/opsmath.cpp 的 rgbToHsl
// （归一化 [0,1)）不同名同义陷阱——本文件版刻意命名 rgbToHslDeg 以示区别。
void rgbToHslDeg(double r, double g, double b, double* h, double* s, double* l) {
  double mx = std::max(r, std::max(g, b));
  double mn = std::min(r, std::min(g, b));
  double dd = mx - mn;
  *l = (mx + mn) / 2.0;
  if (dd <= 0.0) {
    *h = 0.0;
    *s = 0.0;
    return;
  }
  *s = *l > 0.5 ? dd / (2.0 - mx - mn) : dd / (mx + mn);
  if (mx == r) {
    *h = 60.0 * std::fmod((g - b) / dd, 6.0);
  } else if (mx == g) {
    *h = 60.0 * ((b - r) / dd + 2.0);
  } else {
    *h = 60.0 * ((r - g) / dd + 4.0);
  }
  if (*h < 0.0) *h += 360.0;
}

double hueToRgbComponent(double p, double q, double t) {
  if (t < 0.0) t += 1.0;
  if (t > 1.0) t -= 1.0;
  if (t < 1.0 / 6.0) return p + (q - p) * 6.0 * t;
  if (t < 1.0 / 2.0) return q;
  if (t < 2.0 / 3.0) return p + (q - p) * (2.0 / 3.0 - t) * 6.0;
  return p;
}

void hslToRgb(double h, double s, double l, double* r, double* g, double* b) {
  if (s <= 0.0) {
    *r = *g = *b = l;
    return;
  }
  double q = l < 0.5 ? l * (1.0 + s) : l + s - l * s;
  double p = 2.0 * l - q;
  *r = hueToRgbComponent(p, q, h / 360.0 + 1.0 / 3.0);
  *g = hueToRgbComponent(p, q, h / 360.0);
  *b = hueToRgbComponent(p, q, h / 360.0 - 1.0 / 3.0);
}

}  // namespace

void applyHslApprox(Image* image, const HslBands& bands) {
  if (image->pixels.empty() || !bands.any()) return;
  // 近似口径（文档化，非 LR 精确算法；修图台 GPU 路径由 UI B4 实装）：
  // - 权重：色相对 8 个带中心做 45° 半宽三角衰减（环形），多带可叠加
  // - hue：Σ(w·hue)·0.15° 色相偏移；sat：乘子 1 + Σ(w·sat)/100；lum：L + Σ(w·lum)/200
  const double kHalfWidth = 45.0;
  for (size_t i = 0; i + 2 < image->pixels.size(); i += 3) {
    double r = image->pixels[i] / 255.0;
    double g = image->pixels[i + 1] / 255.0;
    double b = image->pixels[i + 2] / 255.0;
    double h = 0.0, s = 0.0, l = 0.0;
    rgbToHslDeg(r, g, b, &h, &s, &l);
    if (s > 0.0) {
      double hueShift = 0.0;
      double satMul = 1.0;
      double lumAdd = 0.0;
      double wSum = 0.0;
      for (int k = 0; k < 8; ++k) {
        double dist = hueDistance(h, kBandCenterHue[k]);
        if (dist >= kHalfWidth) continue;
        double w = 1.0 - dist / kHalfWidth;
        wSum += w;
        hueShift += w * bands.hue[k];
        satMul += w * bands.sat[k] / 100.0;
        lumAdd += w * bands.lum[k] / 200.0;
      }
      if (wSum > 0.0) {
        h += hueShift * 0.15;
        if (h < 0.0) h += 360.0;
        if (h >= 360.0) h -= 360.0;
        s *= satMul;
        if (s < 0.0) s = 0.0;
        if (s > 1.0) s = 1.0;
        l += lumAdd;
        if (l < 0.0) l = 0.0;
        if (l > 1.0) l = 1.0;
        hslToRgb(h, s, l, &r, &g, &b);
      }
    }
    image->pixels[i] = static_cast<uint8_t>(r * 255.0 + 0.5);
    image->pixels[i + 1] = static_cast<uint8_t>(g * 255.0 + 0.5);
    image->pixels[i + 2] = static_cast<uint8_t>(b * 255.0 + 0.5);
  }
}

// ---- 延时 Phase B：3 区参数化 S 曲线 + 颜色分级三色轮 ----

void applyToneCurve(Image* img, const ToneCurve& c) {
  if (img->pixels.empty()) return;
  // 全零 → 恒等变换（提前返回，避免无谓的 HSL 往返量化）
  if (c.shadows == 0.0 && c.midtone == 0.0 && c.highlight == 0.0) return;

  // 各分区位移量（量级与 plan/ops/curve.cpp 的 ParametricOp 一致）
  const double dSh = c.shadows / 100.0 * (60.0 / 255.0);
  const double dMid = c.midtone / 100.0 * (60.0 / 255.0);
  const double dHigh = c.highlight / 100.0 * (60.0 / 255.0);

  // 三分区权重：smooth01 分界 0.25/0.5/0.75，分区归一（权重和 ≡ 1），yNew 始终贴近 y
  for (size_t i = 0; i + 2 < img->pixels.size(); i += 3) {
    const double r0 = img->pixels[i] / 255.0;
    const double g0 = img->pixels[i + 1] / 255.0;
    const double b0 = img->pixels[i + 2] / 255.0;
    double h = 0.0, s = 0.0, l = 0.0;
    rgbToHslDeg(r0, g0, b0, &h, &s, &l);  // h∈[0,360)，l∈[0,1]
    const double y = l;
    const double wSh = 1.0 - smooth01((y - 0.25) / 0.25);
    const double wHigh = smooth01((y - 0.75) / 0.25);
    const double wMid = smooth01((y - 0.25) / 0.25) - smooth01((y - 0.75) / 0.25);
    double yNew = y + wSh * dSh + wMid * dMid + wHigh * dHigh;
    if (yNew < 0.0) yNew = 0.0;
    if (yNew > 1.0) yNew = 1.0;
    // 按亮度曲线调整每像素亮度：保持色相/饱和度，只改 L（HSL-L 重组）
    double r = r0, g = g0, b = b0;
    hslToRgb(h, s, yNew, &r, &g, &b);
    img->pixels[i] = static_cast<uint8_t>(std::clamp(r, 0.0, 1.0) * 255.0 + 0.5);
    img->pixels[i + 1] = static_cast<uint8_t>(std::clamp(g, 0.0, 1.0) * 255.0 + 0.5);
    img->pixels[i + 2] = static_cast<uint8_t>(std::clamp(b, 0.0, 1.0) * 255.0 + 0.5);
  }
}

void applyColorGrade(Image* img, const ColorGrade& g) {
  if (img->pixels.empty()) return;
  // 全零 → 恒等（含 blend/balance 都为 0）
  if (g.shadowHue == 0.0 && g.shadowSat == 0.0 && g.midtoneHue == 0.0 && g.midtoneSat == 0.0 &&
      g.highlightHue == 0.0 && g.highlightSat == 0.0 && g.blend == 0.0 && g.balance == 0.0) {
    return;
  }
  // 照搬 plan/ops/color.cpp 的 ColorGradeOp（line 72-128）3 色轮数学：
  // balance 移动分区边界（0..1 → ±0.15），blend 展宽分区（0..1 → 0..0.25）。
  const double b = g.balance * 0.15;
  const double spread = 0.25 + g.blend * 0.25;
  for (size_t i = 0; i + 2 < img->pixels.size(); i += 3) {
    const double r0 = img->pixels[i] / 255.0;
    const double g0 = img->pixels[i + 1] / 255.0;
    const double b0 = img->pixels[i + 2] / 255.0;
    double h = 0.0, s = 0.0, l = 0.0;
    rgbToHslDeg(r0, g0, b0, &h, &s, &l);  // h∈[0,360)
    double r = r0, gg = g0, bb = b0;
    if (s > 1e-6 || g.shadowSat != 0.0 || g.midtoneSat != 0.0 || g.highlightSat != 0.0) {
      // 三分区权重（balance 上移高光边界，blend 展宽交叠），与 color.cpp 一致
      const double wHi = smooth01(l, 0.5 - b - spread / 2.0, 0.5 - b + spread / 2.0);
      const double wSh = 1.0 - smooth01(l, 0.35 - b - spread / 2.0, 0.35 - b + spread / 2.0);
      const double wMid = std::max(0.0, 1.0 - wHi - wSh);
      struct Zone { double hue; double sat; double w; };
      const Zone zones[3] = {{g.shadowHue, g.shadowSat, wSh},
                             {g.midtoneHue, g.midtoneSat, wMid},
                             {g.highlightHue, g.highlightSat, wHi}};
      double dHue = 0.0, dSat = 0.0;
      for (const Zone& z : zones) {
        if (z.w <= 0.0 || z.sat == 0.0) continue;  // 仅 sat 影响位移（与 color.cpp 一致）
        // 色相域：本模块用角度制 [0,360)，目标角 - 当前角，取最短弧
        double d = z.hue - h;
        if (d > 180.0) d -= 360.0;
        if (d < -180.0) d += 360.0;
        const double strength = std::min(1.0, std::fabs(z.sat) / 100.0) * z.w;
        dHue += d * strength * 0.5;
        dSat += (z.sat / 100.0) * z.w * 0.3;
      }
      if (h != h) h = 0.0;  // NaN 兜底（理论上 rgbToHslDeg 不会产出）
      h += dHue;
      while (h < 0.0) h += 360.0;
      while (h >= 360.0) h -= 360.0;
      s = std::clamp(s * (1.0 + dSat), 0.0, 1.0);
      hslToRgb(h, s, l, &r, &gg, &bb);
    }
    img->pixels[i] = static_cast<uint8_t>(std::clamp(r, 0.0, 1.0) * 255.0 + 0.5);
    img->pixels[i + 1] = static_cast<uint8_t>(std::clamp(gg, 0.0, 1.0) * 255.0 + 0.5);
    img->pixels[i + 2] = static_cast<uint8_t>(std::clamp(bb, 0.0, 1.0) * 255.0 + 0.5);
  }
}

}  // namespace imaging
}  // namespace lumina
