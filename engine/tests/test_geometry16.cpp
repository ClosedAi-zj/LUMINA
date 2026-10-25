// T0.4 测试：重采样核 + 几何单应 + 一次采样断言
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "imaging/image_buffer.h"
#include "imaging/resample.h"
#include "plan/geometry.h"

using lumina::imaging::ImageBuf;
using lumina::imaging::Kernel;
using lumina::plan::applyGeometry;
using lumina::plan::buildForwardHomography;
using lumina::plan::GeometryParams;
using lumina::plan::invert3;
using lumina::imaging::kernelRadius;
using lumina::imaging::kernelWeight;
using lumina::plan::Mat3;

static int g_fail = 0;
#define CHECK(cond, msg)                                              \
  do {                                                                \
    if (!(cond)) {                                                    \
      std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, msg);        \
      ++g_fail;                                                       \
    }                                                                 \
  } while (0)

// 构造平滑渐变图（线性域），避免采样误差被高频淹没
static ImageBuf makeGradient(int w, int h) {
  ImageBuf buf(w, h, lumina::imaging::ColorSpaceTag::SceneLinearWorking);
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      const float v = 0.02f + 0.6f * (x + 1.0f) / w +
                      0.3f * (y + 1.0f) / h;  // [0.02, 0.92] 线性
      uint16_t* p = &buf.data[(static_cast<size_t>(y) * w + x) * 3];
      p[0] = lumina::imaging::log16Encode(v);
      p[1] = lumina::imaging::log16Encode(v * 0.9f);
      p[2] = lumina::imaging::log16Encode(v * 0.8f);
    }
  }
  return buf;
}

// case1：常数图任意重采样/几何 → 常数不变
static void testConstantImage() {
  ImageBuf buf(64, 48, lumina::imaging::ColorSpaceTag::SceneLinearWorking);
  const uint16_t c = lumina::imaging::log16Encode(0.25f);
  for (size_t i = 0; i < buf.data.size(); ++i) buf.data[i] = c;
  for (Kernel k : {Kernel::Box, Kernel::CatmullRom, Kernel::Lanczos3}) {
    ImageBuf out;
    std::string err;
    CHECK(lumina::imaging::resampleImage16(buf, 32, 24, k, &out, &err),
          "resample constant ok");
    CHECK(out.w == 32 && out.h == 24, "resample constant size");
    bool same = true;
    for (uint16_t v : out.data) {
      if (std::abs(static_cast<int>(v) - static_cast<int>(c)) > 1) same = false;
    }
    CHECK(same, "resample constant preserved (±1 LSB)");
  }
  GeometryParams gp;  // identity
  ImageBuf gout;
  std::string gerr;
  uint64_t samples = 0;
  CHECK(applyGeometry(buf, gp, Kernel::CatmullRom, &gout, &samples, &gerr),
        "geometry constant ok");
  bool gsame = true;
  for (uint16_t v : gout.data) {
    if (std::abs(static_cast<int>(v) - static_cast<int>(c)) > 1) gsame = false;
  }
  CHECK(gsame, "geometry constant preserved");
  CHECK(samples == 64ull * 48ull, "geometry constant sampleCount == pixels");
}

// case2：核权重离散归一（网格采样和 ≈ 1）
static void testKernelPartition() {
  for (Kernel k : {Kernel::Box, Kernel::CatmullRom, Kernel::Lanczos3}) {
    const int r = kernelRadius(k);
    const int N = 64;  // 每 1px 单元采样 N 点（中点求积，避免端点重复计数）
    double sum = 0.0;
    for (int i = -r * N; i < r * N; ++i) {
      sum += kernelWeight(k, (i + 0.5) / N) / N;
    }
    CHECK(std::fabs(sum - 1.0) < 5e-3, "kernel partition of unity");
    CHECK(kernelWeight(k, static_cast<double>(r) + 0.1) == 0.0,
          "kernel zero beyond radius");
  }
}

// case3：可分离 vs double 精度非分离参考（同核），PSNR > 45dB
static double psnrAgainstReference(const ImageBuf& src, int dw, int dh,
                                   Kernel k, const ImageBuf& out) {
  // 参考：double 精度直接对每 dst 像素做 2D 窗口核（解码到线性域）
  const int r = kernelRadius(k);
  std::vector<float> lin(src.channelCount());
  lumina::imaging::fromLog16(src.data.data(), lin.data(), src.channelCount(),
                             src.logStep);
  const double scaleX = static_cast<double>(src.w) / dw;
  const double scaleY = static_cast<double>(src.h) / dh;
  const double fx = scaleX < 1.0 ? scaleX : 1.0;
  const double fy = scaleY < 1.0 ? scaleY : 1.0;
  double minV = 0.0;
  for (int dy = 0; dy < dh; ++dy) {
    for (int dx = 0; dx < dw; ++dx) {
      const double cx = (dx + 0.5) * scaleX - 0.5;
      const double cy = (dy + 0.5) * scaleY - 0.5;
      const int x0 = std::max(0, static_cast<int>(std::floor(cx - r / fx)) + 1);
      const int x1 = std::min(src.w, static_cast<int>(std::floor(cx + r / fx)) + 1);
      const int y0 = std::max(0, static_cast<int>(std::floor(cy - r / fy)) + 1);
      const int y1 = std::min(src.h, static_cast<int>(std::floor(cy + r / fy)) + 1);
      double acc[3] = {0.0, 0.0, 0.0};
      double wsum = 0.0;
      for (int sy = y0; sy < y1; ++sy) {
        const double wy = kernelWeight(k, (sy - cy) * fy);
        for (int sx = x0; sx < x1; ++sx) {
          const double wx = kernelWeight(k, (sx - cx) * fx);
          const double wgt = wx * wy;
          wsum += wgt;
          const float* p = &lin[(static_cast<size_t>(sy) * src.w + sx) * 3];
          acc[0] += wgt * p[0];
          acc[1] += wgt * p[1];
          acc[2] += wgt * p[2];
        }
      }
      for (int c = 0; c < 3; ++c) {
        const double ref = acc[c] / wsum;
        const uint16_t got =
            out.data[(static_cast<size_t>(dy) * dw + dx) * 3 + c];
        // 反编码回线性比较
        const double refCode =
            std::exp2(static_cast<double>(
                          lumina::imaging::log16Encode(static_cast<float>(ref),
                                                       out.logStep)) /
                          out.logStep -
                      12.0);
        const double gotLin = lumina::imaging::log16Decode(got, out.logStep);
        const double diff = gotLin - refCode;
        minV += diff * diff;
      }
    }
  }
  const double mse = minV / (static_cast<double>(dw) * dh * 3);
  if (mse < 1e-24) return 999.0;
  return 10.0 * std::log10(1.0 / mse);  // 线性域满幅 ≈ 1.0
}

static void testResamplePsnr() {
  ImageBuf src = makeGradient(97, 61);  // 非整倍尺寸避免巧合
  for (Kernel k : {Kernel::Box, Kernel::CatmullRom, Kernel::Lanczos3}) {
    ImageBuf out;
    std::string err;
    CHECK(lumina::imaging::resampleImage16(src, 43, 27, k, &out, &err),
          "resample ok");
    const double psnr = psnrAgainstReference(src, 43, 27, k, out);
    std::printf("  PSNR[%d] = %.2f dB\n", static_cast<int>(k), psnr);
    CHECK(psnr > 45.0, "separable vs 2D reference PSNR > 45dB");
  }
}

// case4：identity 几何 → 输出与源逐像素一致，采样次数 = 像素数
static void testIdentityGeometry() {
  ImageBuf src = makeGradient(33, 21);
  GeometryParams gp;
  ImageBuf out;
  std::string err;
  uint64_t samples = 0;
  CHECK(applyGeometry(src, gp, Kernel::CatmullRom, &out, &samples, &err),
        "identity geometry ok");
  CHECK(out.w == 33 && out.h == 21, "identity size");
  CHECK(samples == 33ull * 21ull, "identity sampleCount");
  bool same = true;
  for (size_t i = 0; i < src.data.size(); ++i) {
    if (src.data[i] != out.data[i]) same = false;
  }
  CHECK(same, "identity pixel-exact");
}

// case5：90° 旋转（angleDeg=90）→ 尺寸翻转 + 角点对应
static void testRotate90() {
  ImageBuf src = makeGradient(40, 20);
  // 逐像素染色左上角象限便于核对角点
  for (int y = 0; y < 20; ++y) {
    for (int x = 0; x < 40; ++x) {
      uint16_t* p = &src.data[(static_cast<size_t>(y) * 40 + x) * 3];
      p[0] = (x < 20) ? lumina::imaging::log16Encode(0.5f) : 0u;
      p[1] = p[0];
      p[2] = p[0];
    }
  }
  GeometryParams gp;
  gp.angleDeg = 90.0;  // 逆时针 90°：左下角 → 左上方向
  ImageBuf out;
  std::string err;
  uint64_t samples = 0;
  CHECK(applyGeometry(src, gp, Kernel::CatmullRom, &out, &samples, &err),
        "rotate90 ok");
  CHECK(out.w == 20 && out.h == 40, "rotate90 swapped dims");
  CHECK(samples == 20ull * 40ull, "rotate90 sampleCount == dst pixels");
  // 逆时针 90°：src(x,y) → dst(y, W-1-x)；src 左半亮 → dst 上半（y'<20 对应）… 
  // 直接验证：dst 顶行 ≈ src 左列（亮），dst 底行 ≈ src 右列（暗）
  auto rowMean = [&](int row) {
    double s = 0.0;
    for (int x = 0; x < out.w; ++x) {
      s += lumina::imaging::log16Decode(
          out.data[(static_cast<size_t>(row) * out.w + x) * 3], out.logStep);
    }
    return s / out.w;
  };
  const double top = rowMean(1);
  const double bottom = rowMean(out.h - 2);
  CHECK(top > 0.2 && bottom < 0.05, "rotate90 orientation");
}

// case6：裁剪 + 拉直 + 缩放组合 → 采样次数断言 + 输出尺寸正确
static void testCropStraightenScale() {
  ImageBuf src = makeGradient(100, 80);
  GeometryParams gp;
  gp.cropX = 10;
  gp.cropY = 8;
  gp.cropW = 64;
  gp.cropH = 48;
  gp.angleDeg = 5.0;
  gp.perspectiveH = 0.02;
  gp.perspectiveV = -0.01;
  gp.scale = 0.5;
  Mat3 fwd;
  int ow = 0;
  int oh = 0;
  std::string err;
  CHECK(buildForwardHomography(gp, src, &fwd, &ow, &oh, &err), "fwd ok");
  CHECK(ow > 0 && oh > 0, "fwd size positive");
  ImageBuf out;
  uint64_t samples = 0;
  CHECK(applyGeometry(src, gp, Kernel::CatmullRom, &out, &samples, &err),
        "crop+straighten+scale ok");
  CHECK(out.w == ow && out.h == oh, "apply size == build size");
  CHECK(samples == static_cast<uint64_t>(ow) * static_cast<uint64_t>(oh),
        "sampleCount == dst pixels (single sampling)");
}

// case7：奇异单应拒绝 + 非法参数
static void testInvalidParams() {
  ImageBuf src = makeGradient(16, 16);
  GeometryParams gp;
  gp.scale = 0.0;
  ImageBuf out;
  std::string err;
  CHECK(!applyGeometry(src, gp, Kernel::CatmullRom, &out, nullptr, &err),
        "scale<=0 rejected");
  gp.scale = 1.0;
  gp.cropX = 100;  // 越界裁剪
  CHECK(!applyGeometry(src, gp, Kernel::CatmullRom, &out, nullptr, &err),
        "empty crop rejected");
  ImageBuf bad;
  GeometryParams ok;
  CHECK(!applyGeometry(bad, ok, Kernel::CatmullRom, &out, nullptr, &err),
        "invalid src rejected");
  // invert3 对奇异矩阵返回 false
  Mat3 sing;
  sing.m[0] = 1; sing.m[1] = 2; sing.m[2] = 3;
  sing.m[3] = 2; sing.m[4] = 4; sing.m[5] = 6;
  sing.m[6] = 3; sing.m[7] = 6; sing.m[8] = 9;
  Mat3 inv;
  CHECK(!invert3(sing, &inv), "singular invert rejected");
}

int main() {
  testConstantImage();
  testKernelPartition();
  testResamplePsnr();
  testIdentityGeometry();
  testRotate90();
  testCropStraightenScale();
  testInvalidParams();
  if (g_fail == 0) {
    std::printf("test_geometry16: ALL PASS (7 cases)\n");
    return 0;
  }
  std::printf("test_geometry16: %d FAILURES\n", g_fail);
  return 1;
}
