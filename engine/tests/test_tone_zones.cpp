// T1.5b：S2 分区算子锚点语义回归（log16 t 域显示参考标定）。
// 夹具：64x32 四竖带（sRGB 显示口径经 displayToPlanBuf 进域）——
//   白块(255,裁切高光) / 中灰(128) / 深阴影(32) / 近黑(6)。
// 断言（LR 语义，正=提亮、负=回收，与 GPU/预览同号）：
//   highlights-45：白块显著回收（码值显著下降）、中灰影响可忽略、深阴影不动；
//   whites+35：   白块显著提亮、中灰/深阴影不动；
//   shadows+45：  深阴影显著提亮、白块/中灰不动；
//   blacks-35：   近黑显著压暗、中灰/深阴影不动。
// 全部对比基线 = 空参数同管线渲染（链内恒过 pipeline.toneMap，两侧同域可比）。
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "imaging/image_buffer.h"
#include "imaging/imaging.h"
#include "imaging/output_encode.h"  // srgbToLinear（锚点自检换算）
#include "json/json_value.h"
#include "plan/op_table.h"
#include "plan/render_exec.h"

using lumina::imaging::Image;
using lumina::imaging::ImageBuf;
using lumina::json::JsonValue;

namespace {

int g_failed = 0;

#define CHECK(cond, msg)                                      \
  do {                                                        \
    if (!(cond)) {                                            \
      std::printf("  FAIL: %s (line %d)\n", msg, __LINE__);   \
      ++g_failed;                                             \
    }                                                         \
  } while (0)

// 夹具：四条 16px 竖带（白 255 / 中灰 128 / 深阴影 32 / 近黑 6）
Image makeZoneFixture() {
  const int w = 64;
  const int h = 32;
  Image img;
  img.width = w;
  img.height = h;
  img.pixels.resize(static_cast<size_t>(w) * h * 3);
  const uint8_t tones[4] = {255, 128, 32, 6};
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      const uint8_t v = tones[std::min(3, x / 16)];
      const size_t i = (static_cast<size_t>(y) * w + x) * 3;
      img.pixels[i + 0] = v;
      img.pixels[i + 1] = v;
      img.pixels[i + 2] = v;
    }
  }
  return img;
}

// 单参数渲染（与基线同管线：runEditPlan 恒过 pipeline.toneMap）。
// key = EditParams 顶层叶（highlights/whites/shadows/blacks），flatten 负责 basic. 前缀。
bool runZone(const char* key, double amt, ImageBuf* buf, std::string* err) {
  *buf = lumina::plan::displayToPlanBuf(makeZoneFixture());
  JsonValue params = JsonValue::objectValue();
  params.setItem(key, JsonValue::number(amt));
  return lumina::plan::runEditPlan(params, buf, nullptr, err);
}

// 带内采样均值（避免单像素 TPDF/量化毛刺；带中心 ±3 像素）
double bandMean(const ImageBuf& buf, int band) {
  const int w = buf.w;
  double sum = 0.0;
  int n = 0;
  for (int y = 8; y < buf.h - 8; ++y) {
    for (int x = band * 16 + 5; x < band * 16 + 11; ++x) {
      const size_t i = (static_cast<size_t>(y) * w + x) * 3;
      sum += (static_cast<double>(buf.data[i]) + buf.data[i + 1] + buf.data[i + 2]) / 3.0;
      ++n;
    }
  }
  return sum / (n > 0 ? n : 1);
}

void testToneZoneAnchors() {
  std::printf("testToneZoneAnchors\n");
  ImageBuf base;
  std::string err;
  CHECK(runZone("basic.highlights", 0.0, &base, &err), "baseline render");
  const double w0 = bandMean(base, 0);  // 白块
  const double g0 = bandMean(base, 1);  // 中灰
  const double s0 = bandMean(base, 2);  // 深阴影
  const double b0 = bandMean(base, 3);  // 近黑
  // 锚点自检：进域码值 = log16Encode(srgbToLinear(sRGB/255))（toneMap 前口径，
  // 现场换算避免硬编码漂移）。白块 24576 = 显示白锚点 t=0.375。
  const size_t wIdx = (16 * base.w + 8) * 3;
  const size_t gIdx = (16 * base.w + 24) * 3;
  const size_t sIdx = (16 * base.w + 40) * 3;
  const size_t bIdx = (16 * base.w + 56) * 3;
  ImageBuf raw = lumina::plan::displayToPlanBuf(makeZoneFixture());
  auto expectCode = [](uint8_t s) {
    return static_cast<int>(
        lumina::imaging::log16Encode(static_cast<float>(lumina::imaging::srgbToLinear(s / 255.0))));
  };
  CHECK(std::abs(static_cast<int>(raw.data[wIdx]) - expectCode(255)) <= 1,
        "白块码值 = log16(1.0)=24576（显示白锚点 t=0.375）");
  CHECK(std::abs(static_cast<int>(raw.data[gIdx]) - expectCode(128)) <= 1,
        "中灰码值 = 显示 50% 锚点 t≈0.306");
  CHECK(std::abs(static_cast<int>(raw.data[sIdx]) - expectCode(32)) <= 1,
        "深阴影码值 = 显示 ~18% t≈0.184");
  CHECK(std::abs(static_cast<int>(raw.data[bIdx]) - expectCode(6)) <= 1,
        "近黑码值 = 显示 ~2.4% t≈0.091");

  struct Case {
    const char* key;
    double amt;
    int probeBand;
    double maxOtherDelta;  // 非目标区 |Δ| 上限
  };
  // 方向口径（LR，正=提亮）：highlights-45 = 回收（降）；whites+35 = 提；
  // shadows+45 = 提；blacks-35 = 压。目标区断言用 [minDelta, maxDelta] 区间。
  auto runCase = [&](const char* key, double amt, int targetBand, double minDelta,
                     double maxDelta, const int* otherBands, int nOther, double maxOtherAbs,
                     const char* label) {
    ImageBuf z;
    if (!runZone(key, amt, &z, &err)) {
      CHECK(false, (std::string("render: ") + label).c_str());
      return;
    }
    const double dt = bandMean(z, targetBand) - bandMean(base, targetBand);
    CHECK(dt >= minDelta && dt <= maxDelta,
          (std::string(label) + ": 目标区显著变化 (Δ=" + std::to_string(dt) + ")").c_str());
    for (int i = 0; i < nOther; ++i) {
      const double d = std::fabs(bandMean(z, otherBands[i]) - bandMean(base, otherBands[i]));
      CHECK(d <= maxOtherAbs,
            (std::string(label) + ": 非目标区(" + std::to_string(otherBands[i]) +
             ") |Δ|=" + std::to_string(d) + " 可忽略")
                .c_str());
    }
  };

  // highlights-45：白块显著回收（Δ ∈ [-1e9, -400]），中灰 |Δ|≤50（实测 ~1 码，toneMap 压缩后），
  // 深阴影 Δ=0（wHigh(t≈0.184)=0 精确零）
  {
    const int others[] = {1, 2};
    runCase("highlights", -45.0, 0, -1e9, -400.0, others, 2, 50.0, "highlights-45");
  }
  // whites+35：白块显著提亮（Δ ≥ +300），中灰/深阴影精确不动
  {
    const int others[] = {1, 2};
    runCase("whites", 35.0, 0, 300.0, 1e9, others, 2, 0.0, "whites+35");
  }
  // shadows+45：深阴影显著提亮（Δ ≥ +200），白块/中灰精确不动
  {
    const int others[] = {0, 1};
    runCase("shadows", 45.0, 2, 200.0, 1e9, others, 2, 0.0, "shadows+45");
  }
  // blacks-35：近黑显著压暗（Δ ≤ -300），中灰/深阴影精确不动（wBlacks(t≥0.1689)=0）
  {
    const int others[] = {1, 2};
    runCase("blacks", -35.0, 3, -1e9, -300.0, others, 2, 0.0, "blacks-35");
  }
}

}  // namespace

int main() {
  std::string regErr;
  if (!lumina::plan::registerBuiltinOps(&lumina::plan::OpTable::instance(), &regErr)) {
    std::printf("FATAL registerBuiltinOps: %s\n", regErr.c_str());
    return 2;
  }
  testToneZoneAnchors();
  if (g_failed > 0) {
    std::printf("tone_zones: %d CHECK(s) failed\n", g_failed);
    return 1;
  }
  std::printf("tone_zones: all passed\n");
  return 0;
}
