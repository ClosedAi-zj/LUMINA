// T0.7 测试：8bit TPDF 输出编码 + 16bit 透传 + config 缓存 v2
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "core/config.h"
#include "imaging/output_encode.h"

using lumina::imaging::encode16bit;
using lumina::imaging::encode8bit;
using lumina::imaging::ImageBuf;
using lumina::imaging::linearToSrgb;
using lumina::imaging::log16Decode;
using lumina::imaging::log16Encode;
using lumina::imaging::srgbToLinear;

static int g_fail = 0;
#define CHECK(cond, msg)                                       \
  do {                                                         \
    if (!(cond)) {                                             \
      std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, msg); \
      ++g_fail;                                                \
    }                                                          \
  } while (0)

// case1：sRGB 传递函数往返（0..1 步进）
static void testSrgbRoundtrip() {
  for (int i = 0; i <= 1000; ++i) {
    const double v = i / 1000.0;
    const double rt = srgbToLinear(linearToSrgb(v));
    CHECK(std::fabs(rt - v) < 1e-9, "srgb roundtrip");
  }
  CHECK(std::fabs(linearToSrgb(0.5) - 0.7353569830524495) < 1e-9,
        "srgb(0.5) exact");
  CHECK(linearToSrgb(0.0) == 0.0 && linearToSrgb(1.0) == 1.0, "srgb endpoints");
}

// case2：16bit 透传往返精确（任意码值）
static void test16bitPassthrough() {
  ImageBuf buf(16, 16, lumina::imaging::ColorSpaceTag::EncodedSRGB);
  for (size_t i = 0; i < buf.data.size(); ++i) buf.data[i] = static_cast<uint16_t>(i * 7 % 65536);
  std::vector<uint16_t> out;
  std::string err;
  CHECK(encode16bit(buf, &out, &err), "encode16 ok");
  CHECK(out == buf.data, "16bit passthrough exact");
}

// case3：0.5 线性常量图 → 8bit 输出仅 187/188 两值交替、均值 ≈ srgb*255
static void testConstantDither() {
  ImageBuf buf(64, 64, lumina::imaging::ColorSpaceTag::EncodedSRGB);
  const uint16_t code = log16Encode(0.5f);  // 线性 0.5
  for (size_t i = 0; i < buf.data.size(); ++i) buf.data[i] = code;
  std::vector<uint8_t> out;
  std::string err;
  CHECK(encode8bit(buf, &out, &err), "encode8 ok");
  std::vector<int> uniq;
  double sum = 0.0;
  for (uint8_t v : out) {
    sum += v;
    if (std::find(uniq.begin(), uniq.end(), v) == uniq.end()) uniq.push_back(v);
  }
  std::sort(uniq.begin(), uniq.end());
  CHECK(uniq.size() <= 3, "constant dither spans few levels");
  if (!uniq.empty()) {
    CHECK(uniq.front() >= 187 && uniq.back() <= 188, "levels around srgb*255");
  }
  const double mean = sum / out.size();
  const double expect = linearToSrgb(0.5) * 255.0;
  CHECK(std::fabs(mean - expect) < 0.1, "dither mean unbiased");
}

// case4：TPDF 三角分布有界（|d|≤0.5 → 输出永不越过 floor(x*255+0.5)±1）
static void testDitherBounded() {
  ImageBuf buf(32, 32, lumina::imaging::ColorSpaceTag::EncodedSRGB);
  // 极暗 2^-12 → srgb ≈ 0.0003 → 输出 0 或 1，不出现大跳变
  const uint16_t dark = log16Encode(1.0f / 4096.0f);
  for (size_t i = 0; i < buf.data.size(); ++i) buf.data[i] = dark;
  std::vector<uint8_t> out;
  std::string err;
  CHECK(encode8bit(buf, &out, &err), "encode8 dark ok");
  for (uint8_t v : out) CHECK(v <= 1, "dark stays at 0..1");
  // 满幅 → 恒 255（clamp 生效，抖动不越界）
  const uint16_t bright = log16Encode(4.0f);
  for (size_t i = 0; i < buf.data.size(); ++i) buf.data[i] = bright;
  CHECK(encode8bit(buf, &out, &err), "encode8 bright ok");
  for (uint8_t v : out) CHECK(v == 255, "bright clamps to 255");
}

// case5：config 缓存 v2（默认值 + 版本化目录）
static void testCacheV2() {
  CHECK(lumina::core::Config::kCacheFormatVersion == 2, "v2 constant");
  lumina::core::Config cfg;
  CHECK(cfg.cacheFormatVersion() == 2, "default cache version = 2");
  // 未 init 的 Config 渲染目录基于空 home（路径拼接逻辑验证）
  CHECK(cfg.rendersVersionedDir().find("renders") != std::string::npos &&
            cfg.rendersVersionedDir().find("v2") != std::string::npos,
        "versioned renders dir");
}

int main() {
  testSrgbRoundtrip();
  test16bitPassthrough();
  testConstantDither();
  testDitherBounded();
  testCacheV2();
  if (g_fail == 0) {
    std::printf("test_output_encode: ALL PASS (5 cases)\n");
    return 0;
  }
  std::printf("test_output_encode: %d FAILURES\n", g_fail);
  return 1;
}
