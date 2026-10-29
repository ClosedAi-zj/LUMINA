// 通用图像编解码（stb）：JPEG/PNG 解码、缩放、JPEG/PNG 编码、直方图
#ifndef LUMINA_IMAGING_IMAGING_H_
#define LUMINA_IMAGING_IMAGING_H_

#include <cstdint>
#include <string>
#include <vector>

namespace lumina {
namespace imaging {

// 8bit RGB 位图
struct Image {
  int width = 0;
  int height = 0;
  std::vector<uint8_t> pixels;  // RGBRGB...，size = w*h*3
};

// 解码 JPEG/PNG/BMP 等（RAW 请走 raw/raw_decoder.cpp）
bool decodeImageFile(const std::string& path, Image* out, std::string* err);

// 盒式滤波缩放（无需插值参数，质量足够缩略图）
bool resizeImage(const Image& src, Image* dst, int targetWidth, int targetHeight);

bool encodeJpeg(const std::string& path, const Image& image, int quality, std::string* err);
bool encodePng(const std::string& path, const Image& image, std::string* err);

// 直方图（256 桶），edited 管线的近似渲染也用同一入口
// luma 为 nullptr 时行为与旧版一致；非 nullptr 时按 Rec.709 累加亮度桶
void computeHistogram(const Image& image, int64_t r[256], int64_t g[256], int64_t b[256],
                      int64_t* luma = nullptr);

// 近似色调管线：曝光/对比/饱和度（CPU 直方图 / 局部预览用，非修图台渲染）
struct ToneAdjust {
  double exposure = 0.0;   // EV
  double contrast = 0.0;   // -100..100
  double saturation = 0.0;  // -100..100
};
void applyToneApprox(Image* image, const ToneAdjust& adjust);

// 延时 Phase B：3 区参数化 S 曲线（阴影/中间调/高光），逐帧渲染施加到亮度
struct ToneCurve {
  double shadows = 0.0;    // -100..100
  double midtone = 0.0;    // -100..100
  double highlight = 0.0;  // -100..100
};

// 延时 Phase B：颜色分级三色轮（阴影/中间调/高光 色相 + 强度）+ 整体混合/平衡
struct ColorGrade {
  double shadowHue = 0.0, shadowSat = 0.0;      // 色相角度(度) / 强度
  double midtoneHue = 0.0, midtoneSat = 0.0;
  double highlightHue = 0.0, highlightSat = 0.0;
  double blend = 0.0, balance = 0.0;            // 0..1 整体混合 / 分区边界偏移
};

// 3 区参数化 S 曲线：把阴影/中间调/高光三个控制点施加到逐像素亮度（HSL-L 重组，参数 0 ≈ 恒等）
void applyToneCurve(Image* img, const ToneCurve& c);
// 颜色分级三色轮：按亮度三分区权重做 HSL 色相位移 + 饱和乘子（照搬 plan/ops/color.cpp ColorGradeOp）
void applyColorGrade(Image* img, const ColorGrade& g);

// HSL 8 色带近似（WP-A3 导出管线 CPU 路径；修图台 GPU 实装由 UI 侧 B4 负责，参数同源）
// 色带顺序固定：red/orange/yellow/green/aqua/blue/purple/magenta（与协议 hsl 对象键一致）
struct HslBands {
  double hue[8] = {0, 0, 0, 0, 0, 0, 0, 0};  // -100..100
  double sat[8] = {0, 0, 0, 0, 0, 0, 0, 0};  // -100..100
  double lum[8] = {0, 0, 0, 0, 0, 0, 0, 0};  // -100..100

  bool any() const {
    for (int i = 0; i < 8; ++i) {
      if (hue[i] != 0.0 || sat[i] != 0.0 || lum[i] != 0.0) return true;
    }
    return false;
  }
  static int bandIndex(const std::string& name);  // 协议键名 → 下标；未知返回 -1
};
// 单位口径注意：本 imaging 模块的 HSL 色相为角度制 [0,360)（内部实现
// rgbToHslDeg），与 plan/ops/opsmath.cpp 的 rgbToHsl（归一化 [0,1)）不同。
void applyHslApprox(Image* image, const HslBands& bands);

}  // namespace imaging
}  // namespace lumina

#endif  // LUMINA_IMAGING_IMAGING_H_
