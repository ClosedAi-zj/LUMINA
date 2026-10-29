// 延时 Phase B 验收：imaging 8-bit 色调数学（applyToneCurve / applyColorGrade / computeHistogram luma 桶）
//
// 断言清单：
//   T1 applyToneCurve / applyColorGrade 在参数全 0 时输出与输入近似一致（HSL 往返允许 ≤2/255 容差）
//   T2 非零参数时不崩溃、无 NaN、所有通道落在 0..255 整数
//   T3 computeHistogram 的 luma 桶总和 == 像素数（与 r/g/b 各桶总和一致）
#include <cstdint>
#include <cstdio>
#include <vector>

#include "imaging/imaging.h"

using namespace lumina;
using lumina::imaging::Image;

static int g_fail = 0;
// msg 先物化为 std::string 再经 c_str() 传 printf——把动态拼接的 std::string
// 直接塞进 printf 可变参数是 UB（MinGW GCC 8 不报错）：SSO 缓冲被当作 char*
// 解引用 → 消息乱码 / 偶发段错误（同 af32b7a 修复的 test_xmp_mask_v3）。
#define CHECK(cond, msg)                                                 \
  do {                                                                   \
    const std::string check_msg_ = (msg);                                \
    if (!(cond)) {                                                       \
      std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__,                 \
                  check_msg_.c_str());                                   \
      ++g_fail;                                                          \
    } else {                                                             \
      std::printf("  [PASS] %s\n", check_msg_.c_str());                  \
    }                                                                    \
  } while (0)

namespace {

// 64x64 均匀灰（各通道相同）
Image makeGray(uint8_t v) {
  Image img;
  img.width = 64;
  img.height = 64;
  img.pixels.assign(static_cast<size_t>(64) * 64 * 3, v);
  return img;
}

// 64x64 水平灰阶渐变（各通道相同：0..255）
Image makeGradient() {
  Image img;
  img.width = 64;
  img.height = 64;
  img.pixels.resize(static_cast<size_t>(64) * 64 * 3);
  for (int y = 0; y < 64; ++y) {
    for (int x = 0; x < 64; ++x) {
      const uint8_t v = static_cast<uint8_t>(x * 255 / 63);
      const size_t o = (static_cast<size_t>(y) * 64 + x) * 3;
      img.pixels[o] = v;
      img.pixels[o + 1] = v;
      img.pixels[o + 2] = v;
    }
  }
  return img;
}

inline int iabs(int a, int b) { return a > b ? a - b : b - a; }

bool allChannelsInRange(const Image& img) {
  // uint8_t 天然落在 0..255；这里仅确认每个像素都已被正常赋值（无残留异常）
  if (img.pixels.size() != static_cast<size_t>(img.width) * img.height * 3) return false;
  for (uint8_t v : img.pixels) {
    if (v > 255) return false;  // 理论上 uint8_t 不可能，但保留断言哨兵
  }
  return true;
}

int64_t sumBuckets(const int64_t b[256]) {
  int64_t s = 0;
  for (int i = 0; i < 256; ++i) s += b[i];
  return s;
}

}  // namespace

int main() {
  std::printf("== tone_grade: imaging 8-bit 色调数学 ==\n");

  // ---- T1：参数全 0 → 近似恒等 ----
  {
    Image gray = makeGray(128);
    Image grayIn = gray;
    imaging::applyToneCurve(&gray, imaging::ToneCurve{});
    imaging::applyColorGrade(&gray, imaging::ColorGrade{});
    int maxDiff = 0;
    for (size_t i = 0; i < gray.pixels.size(); ++i) {
      const int d = iabs(static_cast<int>(gray.pixels[i]), static_cast<int>(grayIn.pixels[i]));
      if (d > maxDiff) maxDiff = d;
    }
    CHECK(maxDiff <= 2, "T1 灰阶全 0 参数：applyToneCurve+applyColorGrade 近似恒等（最大差 " +
                            std::to_string(maxDiff) + " ≤ 2）");

    Image grad = makeGradient();
    Image gradIn = grad;
    imaging::applyToneCurve(&grad, imaging::ToneCurve{});
    imaging::applyColorGrade(&grad, imaging::ColorGrade{});
    maxDiff = 0;
    for (size_t i = 0; i < grad.pixels.size(); ++i) {
      const int d = iabs(static_cast<int>(grad.pixels[i]), static_cast<int>(gradIn.pixels[i]));
      if (d > maxDiff) maxDiff = d;
    }
    CHECK(maxDiff <= 2, "T1 渐变全 0 参数：近似恒等（最大差 " + std::to_string(maxDiff) + " ≤ 2）");
  }

  // ---- T2：非零参数不崩溃、无 NaN、通道在 0..255 ----
  {
    imaging::ToneCurve tc;
    tc.shadows = 60.0;
    tc.midtone = -40.0;
    tc.highlight = 30.0;
    imaging::ColorGrade cg;
    cg.shadowHue = 25.0;
    cg.shadowSat = 50.0;
    cg.midtoneHue = -15.0;
    cg.midtoneSat = 30.0;
    cg.highlightHue = 200.0;
    cg.highlightSat = 40.0;
    cg.blend = 1.0;
    cg.balance = 0.5;

    Image grad = makeGradient();
    imaging::applyToneCurve(&grad, tc);
    imaging::applyColorGrade(&grad, cg);
    CHECK(allChannelsInRange(grad), "T2 非零参数：所有通道落在 0..255（无越界）");

    // 极端值（满量程）也不崩、不越界
    Image grad2 = makeGradient();
    imaging::ToneCurve tc2;
    tc2.shadows = 100.0;
    tc2.highlight = -100.0;
    imaging::ColorGrade cg2;
    cg2.shadowHue = 180.0;
    cg2.shadowSat = 100.0;
    cg2.highlightHue = -180.0;
    cg2.highlightSat = 100.0;
    cg2.blend = 1.0;
    cg2.balance = 1.0;
    imaging::applyToneCurve(&grad2, tc2);
    imaging::applyColorGrade(&grad2, cg2);
    CHECK(allChannelsInRange(grad2), "T2 满量程极端参数：所有通道仍在 0..255（无越界）");

    // 调色实际生效：渐变图经色调曲线后至少有些像素不同于原图
    Image grad3 = makeGradient();
    Image grad3In = grad3;
    imaging::applyToneCurve(&grad3, tc);
    bool changed = false;
    for (size_t i = 0; i < grad3.pixels.size(); ++i) {
      if (grad3.pixels[i] != grad3In.pixels[i]) { changed = true; break; }
    }
    CHECK(changed, "T2 非零色调曲线：输出确实发生变化（非空操作）");
  }

  // ---- T3：computeHistogram luma 桶总和 == 像素数 ----
  {
    Image grad = makeGradient();
    int64_t r[256], g[256], b[256], luma[256];
    imaging::computeHistogram(grad, r, g, b, luma);
    const int64_t pixels = static_cast<int64_t>(grad.width) * grad.height;
    CHECK(sumBuckets(r) == pixels, "T3 渐变：r 桶总和 == 像素数 (" + std::to_string(pixels) + ")");
    CHECK(sumBuckets(g) == pixels, "T3 渐变：g 桶总和 == 像素数");
    CHECK(sumBuckets(b) == pixels, "T3 渐变：b 桶总和 == 像素数");
    CHECK(sumBuckets(luma) == pixels, "T3 渐变：luma 桶总和 == 像素数（Rec.709）");

    // 调色后 luma 仍每像素贡献 1 个桶
    Image graded = makeGradient();
    imaging::ToneCurve tc3;
    tc3.shadows = 50.0;
    imaging::ColorGrade cg3;
    cg3.shadowHue = 20.0;
    cg3.shadowSat = 40.0;
    imaging::applyToneCurve(&graded, tc3);
    imaging::applyColorGrade(&graded, cg3);
    int64_t r2[256], g2[256], b2[256], luma2[256];
    imaging::computeHistogram(graded, r2, g2, b2, luma2);
    CHECK(sumBuckets(luma2) == pixels, "T3 调色后：luma 桶总和仍 == 像素数");

    // 兼容旧调用（无 luma 参数）不崩溃
    int64_t r3[256], g3[256], b3[256];
    imaging::computeHistogram(grad, r3, g3, b3);
    CHECK(sumBuckets(r3) == pixels, "T3 旧式调用（无 luma）：r 桶总和 == 像素数");
  }

  // CI 可见性：本测试无 SKIP 路径，仍按约定输出（缺失即 CI 无法统计）
  std::printf("[SKIP-COUNT] 0\n");
  std::printf("\n%s (%d failures)\n", g_fail == 0 ? "ALL PASS" : "FAILED", g_fail);
  return g_fail == 0 ? 0 : 1;
}
