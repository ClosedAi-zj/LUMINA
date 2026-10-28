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
  return true;
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

void computeHistogram(const Image& image, int64_t r[256], int64_t g[256], int64_t b[256]) {
  std::memset(r, 0, sizeof(int64_t) * 256);
  std::memset(g, 0, sizeof(int64_t) * 256);
  std::memset(b, 0, sizeof(int64_t) * 256);
  const uint8_t* p = image.pixels.data();
  size_t n = static_cast<size_t>(image.width) * image.height;
  for (size_t i = 0; i < n; ++i) {
    ++r[p[0]];
    ++g[p[1]];
    ++b[p[2]];
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

}  // namespace imaging
}  // namespace lumina
