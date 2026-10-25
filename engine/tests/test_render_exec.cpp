// T1.5：Plan 端到端执行测试（render_exec 唯一实现契约）。
// 覆盖：flattenEditParams 叶路径口径 / compilePlan 含 pipeline.toneMap /
//       runEditPlan 曝光码域精确 / display↔Plan 域往返 / planBufToDisplay TPDF 确定性。
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "imaging/image_buffer.h"
#include "imaging/imaging.h"
#include "json/json_value.h"
#include "plan/op_table.h"
#include "plan/render_exec.h"
#include "plan/ops/ops.h"

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

Image makeGradient(int w, int h) {
  Image img;
  img.width = w;
  img.height = h;
  img.pixels.resize(static_cast<size_t>(w) * h * 3);
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      const size_t i = (static_cast<size_t>(y) * w + x) * 3;
      img.pixels[i + 0] = static_cast<uint8_t>((x * 255) / (w - 1));
      img.pixels[i + 1] = static_cast<uint8_t>((y * 255) / (h - 1));
      img.pixels[i + 2] = static_cast<uint8_t>(((x + y) * 255) / (w + h - 2));
    }
  }
  return img;
}

void testFlatten() {
  std::printf("testFlatten\n");
  const char* kParams = R"({
    "processVersion": "15.4",
    "whiteBalance": "asShot",
    "exposure": 0.5,
    "contrast": 12,
    "calibration": {"shadowTint": 0, "redHue": 5, "redSat": 6},
    "detail": {"sharpness": 20, "sharpenRadius": 1, "sharpenDetail": 25,
               "sharpenMasking": 0, "luminanceSmoothing": 0, "colorNR": 0, "colorNRDetail": 50},
    "geometry": {"hasCrop": false, "cropTop": 0, "cropBottom": 100, "cropLeft": 0,
                 "cropRight": 100, "cropAngle": 0,
                 "perspective": {"vertical": 0, "horizontal": 0, "rotate": 0,
                                  "scale": 100, "aspect": 0, "x": 0, "y": 0}},
    "optics": {"lensProfileEnable": false, "manualDistortion": 0, "vignetteAmount": -18,
               "vignetteMidpoint": 50, "vignetteFeather": 50,
               "grainAmount": 12, "grainFrequency": 20, "grainSize": 3},
    "toneCurveRGB": [{"x": 0, "y": 0}, {"x": 64, "y": 50}, {"x": 255, "y": 255}],
    "parametric": {"highlights": 0, "lights": 0, "darks": 0, "shadows": 0,
                    "shadowSplit": 25, "lightSplit": 50, "midSplit": 75},
    "hsl": {"blue": {"hue": 0, "sat": 25, "lum": 0}},
    "colorGrade": {"shadowHue": 200, "shadowSat": 20, "midtoneHue": 0, "midtoneSat": 0,
                    "highlightHue": 0, "highlightSat": 0, "blend": 60, "balance": 0}
  })";
  JsonValue params;
  std::string parseErr;
  CHECK(JsonValue::parse(kParams, &params, &parseErr), "params parse");
  std::vector<std::pair<std::string, JsonValue>> kvs;
  std::string err;
  CHECK(lumina::plan::flattenEditParams(params, &kvs, &err), "flatten ok");
  // 关键叶必须存在
  auto find = [&](const std::string& path) -> const JsonValue* {
    for (const auto& kv : kvs) {
      if (kv.first == path) return &kv.second;
    }
    return nullptr;
  };
  CHECK(find("basic.exposure") != nullptr, "basic.exposure");
  CHECK(find("basic.whiteBalance") != nullptr, "basic.whiteBalance");
  CHECK(find("calibration.redHue") != nullptr, "calibration.redHue");
  CHECK(find("detail.sharpness") != nullptr, "detail.sharpness");
  CHECK(find("optics.grainAmount") != nullptr, "optics.grainAmount");
  CHECK(find("geometry.hasCrop") != nullptr, "geometry.hasCrop");
  CHECK(find("hsl.blue.sat") != nullptr, "hsl.blue.sat");
  CHECK(find("colorGrade.shadowHue") != nullptr, "colorGrade.shadowHue");
  CHECK(find("parametric.shadowSplit") != nullptr, "parametric.shadowSplit");
  const JsonValue* curve = find("curve.toneCurveRGB");
  CHECK(curve != nullptr && curve->isArray() && curve->size() == 3, "curve.toneCurveRGB 数组原样");
  CHECK(find("processVersion") == nullptr, "processVersion 不产出叶");
  // exposure 值可被 asDouble 消费
  CHECK(std::fabs(find("basic.exposure")->asDouble(999) - 0.5) < 1e-9, "exposure 数值保真");
}

void testCompileToneMap() {
  std::printf("testCompileToneMap\n");
  std::vector<std::pair<std::string, JsonValue>> kvs;
  kvs.push_back({"basic.exposure", JsonValue::number(1.0)});
  lumina::plan::RenderPlan plan;
  std::string err;
  CHECK(lumina::plan::compilePlan(1, kvs, &plan, &err), "compilePlan(JsonValue)");
  bool hasToneMap = false;
  size_t toneMapCount = 0;
  int prevStage = -1;
  for (const std::string& n : plan.opSequence) {
    const lumina::plan::Operator* op = lumina::plan::OpTable::instance().find(n);
    CHECK(op != nullptr, "算子已注册");
    if (op != nullptr) {
      const int s = lumina::plan::stageIndex(op->stage());
      CHECK(s >= prevStage, "stage 有序");
      prevStage = s;
    }
    if (n == "pipeline.toneMap") {
      hasToneMap = true;
      ++toneMapCount;
    }
  }
  CHECK(hasToneMap, "pipeline.toneMap 恒入序列");
  CHECK(toneMapCount == 1, "pipeline.toneMap 去重");
  CHECK(plan.valid(), "三键有效");
  // 空参数也含 toneMap（域分界无条件跨越）
  lumina::plan::RenderPlan emptyPlan;
  std::vector<std::pair<std::string, JsonValue>> none;
  CHECK(lumina::plan::compilePlan(1, none, &emptyPlan, &err), "compilePlan(空)");
  CHECK(emptyPlan.opSequence.size() == 1 && emptyPlan.opSequence[0] == "pipeline.toneMap",
        "空参数序列 = 仅 toneMap");
}

void testRunEditPlanExposure() {
  std::printf("testRunEditPlanExposure\n");
  JsonValue params;
  std::string parseErr;
  CHECK(JsonValue::parse(R"({"exposure": 1.0})", &params, &parseErr), "parse");
  ImageBuf buf(8, 4);
  for (size_t i = 0; i < buf.data.size(); ++i) buf.data[i] = 10000;
  std::string err;
  CHECK(lumina::plan::runEditPlan(params, &buf, nullptr, &err), "runEditPlan");
  // 期望：S2 曝光 +1EV 码值精确 +2048 → S3 toneMap（Reinhard，域分界恒跨越）
  const double lin = lumina::imaging::log16Decode(10000 + 2048, buf.logStep);
  const uint16_t expected =
      lumina::imaging::log16Encode(static_cast<float>(lin / (1.0 + lin)), buf.logStep);
  for (size_t i = 0; i < buf.data.size(); ++i) {
    CHECK(buf.data[i] == expected, "曝光+toneMap 码域精确");
    if (buf.data[i] != expected) break;
  }
  // 对照：无参数时仍过 toneMap（域分界无条件），结果 != 输入
  ImageBuf plain(8, 4);
  for (size_t i = 0; i < plain.data.size(); ++i) plain.data[i] = 10000;
  CHECK(lumina::plan::runEditPlan(JsonValue::objectValue(), &plain, nullptr, &err),
        "runEditPlan(空参数)");
  CHECK(plain.data[0] != 10000, "空参数仍跨越 toneMap 域分界");
  const double lin0 = lumina::imaging::log16Decode(10000, plain.logStep);
  const uint16_t expected0 =
      lumina::imaging::log16Encode(static_cast<float>(lin0 / (1.0 + lin0)), plain.logStep);
  CHECK(plain.data[0] == expected0, "空参数 = 纯 toneMap");
}

void testDisplayRoundtrip() {
  std::printf("testDisplayRoundtrip\n");
  const Image src = makeGradient(64, 48);
  ImageBuf buf = lumina::plan::displayToPlanBuf(src);
  CHECK(buf.valid() && buf.w == 64 && buf.h == 48, "displayToPlanBuf 尺寸");
  Image out;
  std::string err;
  CHECK(lumina::plan::planBufToDisplay(buf, &out, &err), "planBufToDisplay");
  int off = 0;
  int maxDelta = 0;
  for (size_t i = 0; i < src.pixels.size(); ++i) {
    const int d = std::abs(static_cast<int>(out.pixels[i]) - static_cast<int>(src.pixels[i]));
    if (d > 0) ++off;
    if (d > maxDelta) maxDelta = d;
  }
  // 阈值口径：log16 半码量化（相对误差 ~1.7e-4）+ 黑位 clamp（2^-12 地板 → 0→1）。
  // 与预览 encode8 同式同噪，导出/预览两侧一致，此处仅验证往返不发散。
  CHECK(maxDelta <= 1, "域往返 max|Δ| ≤ 1（log16 半码量化）");
  CHECK(off <= static_cast<int>(src.pixels.size()) / 20,
        "域往返 ≥95% 像素逐位一致");
}

void testTpdfDeterministic() {
  std::printf("testTpdfDeterministic\n");
  const Image src = makeGradient(32, 32);
  ImageBuf buf = lumina::plan::displayToPlanBuf(src);
  Image a;
  Image b;
  std::string err;
  CHECK(lumina::plan::planBufToDisplay(buf, &a, &err), "encode A");
  CHECK(lumina::plan::planBufToDisplay(buf, &b, &err), "encode B");
  CHECK(a.pixels == b.pixels, "TPDF 确定性：两次编码逐位一致");
}

}  // namespace

int main() {
  // 注册算子（compilePlan 的 stage 排序依赖 OpTable；runEditPlan 内部也会注册）
  std::string regErr;
  if (!lumina::plan::registerBuiltinOps(&lumina::plan::OpTable::instance(), &regErr)) {
    std::printf("FATAL registerBuiltinOps: %s\n", regErr.c_str());
    return 2;
  }
  testFlatten();
  testCompileToneMap();
  testRunEditPlanExposure();
  testDisplayRoundtrip();
  testTpdfDeterministic();
  if (g_failed > 0) {
    std::printf("render_exec: %d CHECK(s) failed\n", g_failed);
    return 1;
  }
  std::printf("render_exec: all passed\n");
  return 0;
}
