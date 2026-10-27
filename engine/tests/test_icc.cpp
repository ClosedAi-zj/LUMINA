// M4-01 色彩管理回归：ICC 构造/序列化/转换往返（sRGB ↔ AdobeRGB）
// 产物：m4-icc-roundtrip.json（真实测量值，不做修饰）
//
// 口径声明（重要）：
//  - 往返判据按 PRD「sRGB→AdobeRGB→sRGB 往返 mean|Δ| = 0、max ≤ 1 LSB」执行，
//    中间态用 16bit 承载；8bit 中间态的损耗另立字段如实记录，不与 16bit 指标混报。
//  - 软打样一节只做**实现级冒烟 + 描述性统计**。PRD 的 ΔE00 验收需要以 Photoshop 软打样
//    或真实印刷 profile 为参考，本机两者都不具备，故报告里 acceptance 明确标为 not_verified，
//    不冒充通过。
#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "color/icc.h"

using namespace lumina::color;

namespace {

int g_fail = 0;

void check(bool cond, const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  std::printf(cond ? "  \xE2\x9C\x93 " : "  \xE2\x9C\x97 ");
  std::vfprintf(stdout, fmt, ap);
  std::printf("\n");
  va_end(ap);
  if (!cond) ++g_fail;
}

/** 覆盖全部 8bit 组合的合成样张：R 横向渐变、G 纵向渐变、B 棋盘 */
std::vector<uint8_t> makeGradient(int w, int h) {
  std::vector<uint8_t> px(static_cast<size_t>(w) * h * 3);
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      const size_t i = (static_cast<size_t>(y) * w + x) * 3;
      px[i + 0] = static_cast<uint8_t>(x * 255 / (w > 1 ? w - 1 : 1));
      px[i + 1] = static_cast<uint8_t>(y * 255 / (h > 1 ? h - 1 : 1));
      px[i + 2] = static_cast<uint8_t>(((x / 16 + y / 16) % 2) ? 240 : 16);
    }
  }
  return px;
}

/** 可复现伪随机样张（固定 LCG，避免运行间漂移） */
std::vector<uint8_t> makeNoise(int w, int h, uint32_t seed) {
  std::vector<uint8_t> px(static_cast<size_t>(w) * h * 3);
  uint32_t s = seed ? seed : 1u;
  for (size_t i = 0; i < px.size(); ++i) {
    s = s * 1664525u + 1013904223u;
    px[i] = static_cast<uint8_t>((s >> 16) & 0xFF);
  }
  return px;
}

bool hasAcsp(const std::vector<uint8_t>& p) {
  return p.size() > 40 && p[36] == 'a' && p[37] == 'c' && p[38] == 's' && p[39] == 'p';
}

}  // namespace

int main() {
  std::printf("== M4-01 ICC 色彩管理 ==\n");

  const bool lcmsOk = available();
  check(lcmsOk, "lcms2 可用");

  std::vector<uint8_t> srgb, adobe;
  std::string err;
  // 注意：不能把 srgb.size() 直接写进 check(...) 的实参里 —— C++ 不保证实参求值顺序，
  // 那样打印出来的会是调用前的尺寸（本次自测就踩到过，显示 0 字节）。先调用，再读尺寸。
  const bool okSrgb = profileBytes(ProfileId::Srgb, &srgb, &err);
  check(okSrgb, "sRGB profile 序列化（%zu 字节）", srgb.size());
  err.clear();
  const bool okAdobe = profileBytes(ProfileId::AdobeRgb, &adobe, &err);
  check(okAdobe, "AdobeRGB profile 序列化（%zu 字节）", adobe.size());
  check(hasAcsp(srgb), "sRGB profile 头含 'acsp' 签名");
  check(hasAcsp(adobe), "AdobeRGB profile 头含 'acsp' 签名");
  check(!srgb.empty() && !adobe.empty() && srgb != adobe, "两个 profile 内容不同");

  // ---- 往返：sRGB → AdobeRGB(16bit 中间) → sRGB ----
  const int w = 256, h = 64;
  std::vector<uint8_t> src = makeGradient(w, h);
  RoundtripStats st;
  err.clear();
  const bool rtOk = measureRoundtrip(src.data(), w, h, ProfileId::Srgb, ProfileId::AdobeRgb, &st,
                                      &err);
  check(rtOk, "16bit 中间态往返执行成功 %s", err.c_str());
  check(rtOk && st.maxAbs <= 1, "往返 max|Δ| ≤ 1 LSB（实测 %d）", st.maxAbs);
  check(rtOk && st.meanAbs == 0.0, "往返 mean|Δ| = 0（实测 %.6f）", st.meanAbs);

  // ---- 8bit 直存口径（导出 8bit AdobeRGB 文件的真实往返损耗）----
  std::vector<uint8_t> mid8, back8;
  err.clear();
  const bool s1 = convertRgb8(src.data(), w, h, ProfileId::Srgb, ProfileId::AdobeRgb, &mid8, &err);
  const bool s2 = s1 && convertRgb8(mid8.data(), w, h, ProfileId::AdobeRgb, ProfileId::Srgb,
                                    &back8, &err);
  double mean8 = 0.0;
  int max8 = 0;
  long long nz8 = 0;
  if (s2) {
    long long sum = 0;
    for (size_t i = 0; i < src.size(); ++i) {
      const int d = std::abs(static_cast<int>(src[i]) - static_cast<int>(back8[i]));
      sum += d;
      if (d) ++nz8;
      if (d > max8) max8 = d;
    }
    mean8 = static_cast<double>(sum) / static_cast<double>(src.size());
  }
  check(s2, "8bit 中间态往返执行成功");
  std::printf("  · 8bit 中间态往返：mean|Δ| = %.4f LSB, max|Δ| = %d LSB, 非零样本 %lld/%zu\n", mean8,
              max8, nz8, src.size());

  // ---- 灰阶：往返后不得出现可见偏移与通道染色 ----
  std::vector<uint8_t> gray(static_cast<size_t>(256) * 3);
  for (int i = 0; i < 256; ++i) {
    gray[i * 3 + 0] = gray[i * 3 + 1] = gray[i * 3 + 2] = static_cast<uint8_t>(i);
  }
  std::vector<uint8_t> grayAdobe, grayRound;
  std::string gerr;
  const bool g1 = convertRgb8(gray.data(), 256, 1, ProfileId::Srgb, ProfileId::AdobeRgb, &grayAdobe,
                              &gerr);
  int maxChroma = 0;
  if (g1) {
    for (int i = 0; i < 256; ++i) {
      const int r = grayAdobe[i * 3 + 0], g = grayAdobe[i * 3 + 1], b = grayAdobe[i * 3 + 2];
      const int spread = std::max(std::abs(r - g), std::max(std::abs(g - b), std::abs(r - b)));
      if (spread > maxChroma) maxChroma = spread;
    }
  }
  check(g1, "灰阶 sRGB→AdobeRGB 转换执行成功");
  // AdobeRGB 与 sRGB 基色不同，同一绝对色的数值不完全相等属正常；用户可见的判据是
  // 「回到 sRGB 后灰阶是否还原」，故染色判定放在回程。
  const bool g2 = g1 && convertRgb8(grayAdobe.data(), 256, 1, ProfileId::AdobeRgb, ProfileId::Srgb,
                                    &grayRound, &gerr);
  int maxGrayDrift = 0, maxGraySpread = 0;
  if (g2) {
    for (int i = 0; i < 256; ++i) {
      const int d = std::abs(static_cast<int>(gray[i * 3]) - static_cast<int>(grayRound[i * 3]));
      if (d > maxGrayDrift) maxGrayDrift = d;
      const int r = grayRound[i * 3 + 0], g = grayRound[i * 3 + 1], b = grayRound[i * 3 + 2];
      const int spread = std::max(std::abs(r - g), std::max(std::abs(g - b), std::abs(r - b)));
      if (spread > maxGraySpread) maxGraySpread = spread;
    }
  }
  check(g2, "灰阶往返执行成功");
  check(g2 && maxGrayDrift <= 1, "灰阶往返无可见偏移（实测 max %d LSB）", maxGrayDrift);
  check(g2 && maxGraySpread == 0, "灰阶往返零染色（实测通道最大差 %d）", maxGraySpread);
  std::printf("  · AdobeRGB 空间内灰阶通道最大差 = %d（基色不同导致的正常数值差）\n", maxChroma);

  // ---- 软打样：实现冒烟 + 描述性统计（不冒充 PRD 验收）----
  struct Sample {
    int index;
    double mean;
    double median;
    double p95;
    double max;
  };
  std::vector<Sample> samples;
  for (int k = 0; k < 10; ++k) {
    const int sw = 64, sh = 48;
    std::vector<uint8_t> img =
        (k % 2 == 0) ? makeGradient(sw, sh) : makeNoise(sw, sh, 0x9E3779B9u + static_cast<uint32_t>(k));
    SoftProofOptions opts;  // 默认目标 = PrintSim（合成印刷色域），会产生真实裁切
    opts.simulatePaper = (k % 3 != 0);
    std::vector<uint8_t> proof;
    std::string sperr;
    if (!softProofRgb8(img.data(), sw, sh, opts, &proof, &sperr)) {
      check(false, "样本 %d 软打样失败: %s", k, sperr.c_str());
      continue;
    }
    std::vector<double> labProof, labSrc;
    std::string lerr;
    if (!srgb8ToLab(proof.data(), sw, sh, &labProof, &lerr) ||
        !srgb8ToLab(img.data(), sw, sh, &labSrc, &lerr)) {
      check(false, "样本 %d Lab 转换失败: %s", k, lerr.c_str());
      continue;
    }
    std::vector<double> des;
    des.reserve(static_cast<size_t>(sw) * sh);
    for (size_t i = 0; i < static_cast<size_t>(sw) * sh; ++i) {
      des.push_back(deltaE00(&labProof[i * 3], &labSrc[i * 3]));
    }
    std::sort(des.begin(), des.end());
    double sum = 0.0;
    for (double d : des) sum += d;
    Sample r;
    r.index = k;
    r.mean = sum / static_cast<double>(des.size());
    r.median = des[des.size() / 2];
    r.p95 = des[static_cast<size_t>(des.size() * 95 / 100)];
    r.max = des.back();
    samples.push_back(r);
  }
  check(samples.size() == 10, "软打样 10 组样本执行成功（实际 %zu）", samples.size());

  // 打样必须真的改变画面（否则就是"控件摆设"）：PrintSim 目标应让饱和色裁切
  double maxMedian = 0.0;
  double minMedian = 1e9;
  for (const auto& s : samples) {
    if (s.median > maxMedian) maxMedian = s.median;
    if (s.median < minMedian) minMedian = s.median;
  }
  check(samples.size() == 10 && maxMedian > 0.5,
        "PrintSim 软打样产生真实色彩位移（样本中位 ΔE 最大 %.3f / 最小 %.3f）", maxMedian,
        samples.size() ? minMedian : 0.0);

  // ---- 写报告 ----
  FILE* f = std::fopen("m4-icc-roundtrip.json", "wb");
  if (!f) {
    std::printf("  \xE2\x9C\x97 无法写 m4-icc-roundtrip.json\n");
    ++g_fail;
  } else {
    std::fprintf(f, "{\n");
    std::fprintf(f, "  \"task\": \"M4-01\",\n");
    std::fprintf(f, "  \"engine\": \"lcms2 2.16 (vendored, MIT)\",\n");
    std::fprintf(f, "  \"profiles\": {\"srgb\": \"%s\", \"adobeRgb\": \"%s\"},\n",
                 profileName(ProfileId::Srgb), profileName(ProfileId::AdobeRgb));
    std::fprintf(f,
                 "  \"acceptance\": {\"criterion\": \"sRGB→AdobeRGB→sRGB mean|d|=0, max<=1 LSB\", "
                 "\"pass\": %s},\n",
                 (rtOk && st.meanAbs == 0.0 && st.maxAbs <= 1) ? "true" : "false");
    std::fprintf(
        f,
        "  \"roundtrip16\": {\"meanAbs\": %.6f, \"maxAbs\": %d, \"samples\": %lld, \"nonZero\": %lld},\n",
        st.meanAbs, st.maxAbs, st.samples, st.nonZero);
    std::fprintf(f, "  \"roundtrip8\": {\"meanAbs\": %.6f, \"maxAbs\": %d, \"nonZero\": %lld},\n", mean8,
                 max8, nz8);
    std::fprintf(f, "  \"grayRoundtrip\": {\"maxDrift\": %d, \"maxChannelSpread\": %d},\n",
                 maxGrayDrift, maxGraySpread);
    std::fprintf(f,
                 "  \"softProof\": {\"acceptance\": \"not_verified\", \"reason\": "
                 "\"PRD 要求以 Photoshop 软打样/真实印刷 ICC 为参考；本机两者均不具备，"
                 "此处仅为实现冒烟与描述性统计\", \"metric\": \"dE00 vs 原图\", \"samples\": [\n");
    for (size_t i = 0; i < samples.size(); ++i) {
      std::fprintf(
          f, "    {\"index\": %d, \"mean\": %.4f, \"median\": %.4f, \"p95\": %.4f, \"max\": %.4f}%s\n",
          samples[i].index, samples[i].mean, samples[i].median, samples[i].p95, samples[i].max,
          (i + 1 < samples.size()) ? "," : "");
    }
    std::fprintf(f, "  ]}\n}\n");
    std::fclose(f);
    std::printf("  · 报告已写 m4-icc-roundtrip.json\n");
  }

  // ---- M4-03 产物：软打样 ΔE 报告（与 M4-01 分开命名，便于 CI 按件追溯）----
  FILE* sp = std::fopen("m4-softproof-delta-e.json", "wb");
  if (!sp) {
    std::printf("  \xE2\x9C\x97 无法写 m4-softproof-delta-e.json\n");
    ++g_fail;
  } else {
    std::fprintf(sp, "{\n  \"task\": \"M4-03\",\n");
    std::fprintf(sp, "  \"engine\": \"lcms2 cmsCreateProofingTransform + SOFTPROOFING\",\n");
    std::fprintf(sp, "  \"target\": \"%s\",\n", profileName(ProfileId::PrintSim));
    std::fprintf(sp, "  \"targetNote\": \"合成印刷色域（非真实纸墨 ICC）；真实印刷请用打印机 ICC 覆盖\",\n");
    std::fprintf(sp,
                 "  \"acceptance\": {\"prd\": \"preview vs 参考 ICC transform：ΔE00 中位<=2.0、95分位<=4.0\", "
                 "\"status\": \"not_verified\", "
                 "\"reason\": \"PRD 的参考对象是 Photoshop 软打样/真实印刷 profile，本环境不具备；"
                 "本报告为『打样后 vs 打样前』的描述性统计\"},\n");
    std::fprintf(sp, "  \"metric\": \"dE00(proofed, original) over sRGB Lab\",\n");
    std::fprintf(sp, "  \"samples\": [\n");
    for (size_t i = 0; i < samples.size(); ++i) {
      std::fprintf(
          sp, "    {\"index\": %d, \"mean\": %.4f, \"median\": %.4f, \"p95\": %.4f, \"max\": %.4f}%s\n",
          samples[i].index, samples[i].mean, samples[i].median, samples[i].p95, samples[i].max,
          (i + 1 < samples.size()) ? "," : "");
    }
    std::fprintf(sp, "  ],\n  \"visibleChange\": %s\n}\n",
                 (maxMedian > 0.5) ? "true" : "false");
    std::fclose(sp);
    std::printf("  · 报告已写 m4-softproof-delta-e.json\n");
  }

  std::printf(g_fail == 0 ? "M4-01 PASS\n" : "M4-01 FAIL (%d)\n", g_fail);
  return g_fail == 0 ? 0 : 1;
}
