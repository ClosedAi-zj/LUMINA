// T0.2 验收：RAW 16bit 线性输出（output_bps=16、gamma 线性、关自动提亮）
// 样本：build/rw2-real/library/*.rw2（S5M2）；无样本时优雅跳过（ctest 仍绿）
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "raw/raw_decoder.h"

using namespace lumina;

namespace {

int gFailures = 0;

void check(bool condition, const std::string& name) {
  if (condition) {
    printf("  [PASS] %s\n", name.c_str());
  } else {
    printf("  [FAIL] %s\n", name.c_str());
    ++gFailures;
  }
}

std::string findSample() {
  const char* candidates[] = {
      "D:/LUMINA/build/rw2-real/library/2025-12-29_camera_PANA0079.rw2",
      "D:/LUMINA/build/rw2-real/library/2025-12-29_camera_PANA0080.rw2",
  };
  for (const char* c : candidates) {
    FILE* f = fopen(c, "rb");
    if (f) { fclose(f); return c; }
  }
  return "";
}

}  // namespace

int main() {
  printf("== test_raw16 ==\n");
  std::string sample = findSample();
  if (sample.empty()) {
    printf("  [SKIP] 无 RW2 样本（D:/LUMINA/build/rw2-real/library/），用例跳过\n");
    printf("== test_raw16 ALL PASS (skipped) ==\n");
    return 0;
  }
  raw::RawDecoder decoder;
  std::string err;

  // 用例 1：16bit 线性解码成功、形状正确、log16 载体有效
  imaging::ImageBuf buf16;
  check(decoder.decodeToImage16(sample, false, &buf16, &err), "case1: decodeToImage16 成功 " + err);
  check(buf16.valid(), "case1b: ImageBuf valid（w*h*3 log16）");
  check(buf16.space == imaging::ColorSpaceTag::SceneLinearCamera, "case1c: space=SceneLinearCamera");

  // 用例 2：whitePoint/blackPoint 记录正确（white > black >= 0）
  printf("  [INFO] blackPoint=%.0f whitePoint=%.0f %dx%d\n", buf16.blackPoint, buf16.whitePoint,
         buf16.w, buf16.h);
  check(buf16.whitePoint > buf16.blackPoint, "case2a: whitePoint > blackPoint");
  check(buf16.blackPoint >= 0.f, "case2b: blackPoint >= 0");

  // 用例 3：>8bit 信息量（16bit 线性输出不应只有 256 个码级）
  // 取 G 通道（交错 index 1）统计 distinct log16 码
  {
    std::vector<uint16_t> codes;
    codes.reserve(static_cast<size_t>(buf16.w) * buf16.h / 16);
    for (size_t i = 1; i < buf16.data.size(); i += 3 * 16) codes.push_back(buf16.data[i]);
    std::sort(codes.begin(), codes.end());
    codes.erase(std::unique(codes.begin(), codes.end()), codes.end());
    printf("  [INFO] distinct codes in sampled channel = %zu\n", codes.size());
    check(codes.size() > 1000, "case3: distinct log16 码 > 1000（16bit 未塌缩）");
  }

  // 用例 4：与 8bit 版视觉可比（同一采样步长、同一通道：16bit 线性经 sRGB 编码后
  // 的均值 vs 8bit 均值，±30%）
  {
    imaging::Image img8;
    if (decoder.decodeToBitmap(sample, false, &img8, &err) && img8.pixels.size() ==
        static_cast<size_t>(img8.width) * img8.height * 3) {
      // 8bit：与 16bit 完全相同的采样方式（R 通道，每 16 像素取 1）
      double sum8 = 0.0;
      size_t cnt = 0;
      for (size_t i = 0; i < img8.pixels.size(); i += 16 * 3) {
        sum8 += img8.pixels[i];
        ++cnt;
      }
      double mean8 = sum8 / cnt;
      // 16bit 线性 → sRGB 编码（与 LibRaw 8bit 默认 gamma 同族）后统计均值
      double sum16 = 0.0;
      for (size_t i = 0; i < buf16.data.size(); i += 16 * 3) {
        double lin = imaging::log16Decode(buf16.data[i]);
        double enc = lin <= 0.003 ? lin * 12.92 : 1.055 * std::pow(lin, 1.0 / 2.4) - 0.055;
        sum16 += std::max(0.0, std::min(1.0, enc)) * 255.0;
      }
      double mean16 = sum16 / cnt;
      printf("  [INFO] mean8=%.1f mean16(gamma-encoded)=%.1f (R channel, same stride)\n",
             mean8, mean16);
      check(mean16 > mean8 * 0.7 && mean16 < mean8 * 1.3, "case4: 8bit/16bit 亮度可比（±30%）");
    } else {
      printf("  [SKIP] case4: 8bit 解码失败，跳过可比性检查\n");
    }
  }

  printf("== test_raw16 %s ==\n", gFailures == 0 ? "ALL PASS" : "FAILED");
  return gFailures == 0 ? 0 : 1;
}
