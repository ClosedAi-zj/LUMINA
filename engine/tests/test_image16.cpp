// T0.1 验收：log16 编解码 65536 值全域往返 max rel err < 0.1% + ImageBuf 结构
#include <cmath>
#include <cstdio>
#include <string>

#include "imaging/image_buffer.h"

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

}  // namespace

int main() {
  printf("== test_image16 ==\n");
  const int32_t kLogStep = 2048;

  // 用例 1：全域码值往返（u → decode → encode → u'），必须逐码精确还原
  {
    bool exact = true;
    for (int u = 0; u <= 65535; ++u) {
      uint16_t back = imaging::log16Encode(imaging::log16Decode(
          static_cast<uint16_t>(u), kLogStep), kLogStep);
      if (back != u) {
        exact = false;
        printf("  [FAIL] roundtrip u=%d back=%u\n", u, back);
        break;
      }
    }
    check(exact, "case1: 65536 码值 decode→encode 逐码精确还原");
  }

  // 用例 2：浮点全域相对误差（扫 2^-12 .. 2^20 对数均匀 + 线性扫暗部）
  {
    double maxRel = 0.0;
    for (int i = 0; i <= 200000; ++i) {
      // 对数均匀采样 32 个 octave（2^-12 .. 2^20）
      double v = std::exp2(-12.0 + 32.0 * i / 200000.0);
      float d = imaging::log16Decode(imaging::log16Encode(static_cast<float>(v), kLogStep), kLogStep);
      double rel = std::fabs(d - v) / v;
      if (rel > maxRel) maxRel = rel;
    }
    // 线性细扫深阴影（条带最敏感区）
    for (int i = 0; i <= 100000; ++i) {
      double v = (1.0 / 4096.0) + (1.0 / 256.0 - 1.0 / 4096.0) * i / 100000.0;
      float d = imaging::log16Decode(imaging::log16Encode(static_cast<float>(v), kLogStep), kLogStep);
      double rel = std::fabs(d - v) / v;
      if (rel > maxRel) maxRel = rel;
    }
    printf("  [INFO] max rel err = %.6f%%\n", maxRel * 100.0);
    check(maxRel < 0.001, "case2: 浮点全域 max rel err < 0.1%（实测见上）");
    check(maxRel < 0.0005, "case2b: 达成 0.05% 档宽设计目标");
  }

  // 用例 3：端点与饱和语义
  {
    check(imaging::log16Encode(0.0f, kLogStep) == 0, "case3a: v<=0 → 码 0");
    check(imaging::log16Encode(-1.0f, kLogStep) == 0, "case3b: 负值 → 码 0");
    check(imaging::log16Encode(1.0f / 4096.0f, kLogStep) == 0, "case3c: 2^-12 → 码 0");
    // 1.0（中灰参考）应为 12*2048 = 24576
    check(imaging::log16Encode(1.0f, kLogStep) == 24576, "case3d: 1.0 → 12*2048=24576");
    check(imaging::log16Encode(1e9f, kLogStep) == 65535, "case3e: 超域 → 饱和 65535");
  }

  // 用例 4：每 octave 恰 2048 码 → 相对步进恒定 ≈0.034%
  {
    double relStep = std::exp2(1.0 / kLogStep) - 1.0;
    printf("  [INFO] per-code rel step = %.6f%%\n", relStep * 100.0);
    check(relStep < 0.0005, "case4: 单码相对步进 < 0.05%");
  }

  // 用例 5：ImageBuf 结构与形状
  {
    imaging::ImageBuf buf(4, 3);
    check(buf.valid(), "case5a: 构造后 valid");
    check(buf.data.size() == 4 * 3 * 3, "case5b: data = w*h*3 交错");
    check(buf.space == imaging::ColorSpaceTag::SceneLinearCamera, "case5c: 默认 SceneLinearCamera");
    check(buf.logStep == 2048 && buf.iccProfileId == -1, "case5d: logStep=2048 / icc=-1 默认");
    buf.data[0] = imaging::log16Encode(0.25f);
    check(imaging::log16Decode(buf.data[0]) > 0.249f &&
              imaging::log16Decode(buf.data[0]) < 0.251f, "case5e: 存取经 log16 保真");
    imaging::ImageBuf bad;
    check(!bad.valid(), "case5f: 默认构造 invalid");
  }

  // 用例 6：批量接口与逐点一致
  {
    float src[6] = {0.001f, 0.01f, 0.1f, 1.0f, 4.0f, 15.0f};
    uint16_t enc[6];
    float dec[6];
    imaging::toLog16(src, enc, 6);
    imaging::fromLog16(enc, dec, 6);
    bool ok = true;
    for (int i = 0; i < 6; ++i) {
      float single = imaging::log16Decode(imaging::log16Encode(src[i]));
      if (single != dec[i]) ok = false;
    }
    check(ok, "case6: 批量 toLog16/fromLog16 与逐点一致");
  }

  printf("== test_image16 %s ==\n", gFailures == 0 ? "ALL PASS" : "FAILED");
  return gFailures == 0 ? 0 : 1;
}
