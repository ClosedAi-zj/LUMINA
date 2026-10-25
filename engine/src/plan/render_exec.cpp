#include "plan/render_exec.h"

#include <cmath>
#include <mutex>
#include <vector>

#include "imaging/output_encode.h"
#include "plan/op_table.h"
#include "plan/ops/ops.h"  // hashNoise（确定性 TPDF 噪源，与预览 encode8 同源）

namespace lumina {
namespace plan {

namespace {

// 数值稳定输出取整（与 oracle93 encode8 / FRAG_EFFECTS 同式）
inline uint8_t quantize8(double enc01, double dither) {
  const double v = std::floor(enc01 * 255.0 + 0.5 + dither);
  if (v < 0.0) return 0;
  if (v > 255.0) return 255;
  return static_cast<uint8_t>(v);
}

// 标量叶发射（仅接受 string/number/bool；对象/数组由专用分支处理）
void emitScalar(const json::JsonValue& obj, const char* key, const std::string& path,
                std::vector<std::pair<std::string, json::JsonValue>>* out) {
  const json::JsonValue v = obj.get(key);
  if (v.isString() || v.isNumber() || v.isBool()) out->push_back({path, v});
}

// 嵌套对象逐叶发射（calibration/detail/optics/colorGrade/parametric 口径）
void emitObjectLeaves(const json::JsonValue& obj, const char* prefix,
                      std::vector<std::pair<std::string, json::JsonValue>>* out) {
  if (!obj.isObject()) return;
  for (const auto& kv : obj.objectItems()) {
    if (kv.second.isString() || kv.second.isNumber() || kv.second.isBool()) {
      out->push_back({std::string(prefix) + kv.first, kv.second});
    }
  }
}

}  // namespace

imaging::ImageBuf displayToPlanBuf(const imaging::Image& image) {
  imaging::ImageBuf buf(image.width, image.height,
                        imaging::ColorSpaceTag::SceneLinearWorking);
  const size_t n = static_cast<size_t>(image.width) * static_cast<size_t>(image.height);
  for (size_t i = 0; i < n; ++i) {
    for (int c = 0; c < 3; ++c) {
      const double s = static_cast<double>(image.pixels[i * 3 + c]) / 255.0;
      buf.data[i * 3 + c] =
          imaging::log16Encode(static_cast<float>(imaging::srgbToLinear(s)));
    }
  }
  return buf;
}

bool planBufToDisplay(const imaging::ImageBuf& buf, imaging::Image* out, std::string* err) {
  if (!out) {
    if (err) *err = "planBufToDisplay: null out";
    return false;
  }
  if (!buf.valid()) {
    if (err) *err = "planBufToDisplay: invalid buffer";
    return false;
  }
  out->width = buf.w;
  out->height = buf.h;
  out->pixels.assign(buf.data.size(), 0);
  for (int y = 0; y < buf.h; ++y) {
    for (int x = 0; x < buf.w; ++x) {
      const size_t i = (static_cast<size_t>(y) * buf.w + x) * 3;
      for (int c = 0; c < 3; ++c) {
        const double lin = imaging::log16Decode(buf.data[i + c], buf.logStep);
        const double enc = imaging::linearToSrgb(lin);
        // 确定性 TPDF（log16.ts tpdfDither 精确镜像：双哈希均值 - 0.5，通道无关）
        const double u1 = hashNoise(x, y, 1000);
        const double u2 = hashNoise(x + 7919, y + 104729, 2000);
        out->pixels[i + c] = quantize8(enc, (u1 + u2) * 0.5 - 0.5);
      }
    }
  }
  return true;
}

bool flattenEditParams(const json::JsonValue& params,
                       std::vector<std::pair<std::string, json::JsonValue>>* out,
                       std::string* err) {
  if (!out) {
    if (err) *err = "flattenEditParams: null out";
    return false;
  }
  if (!params.isObject()) {
    if (err) *err = "flattenEditParams: params 必须是对象（EditParams）";
    return false;
  }
  out->clear();
  // ---- basic（顶层标量 14 叶 + WB 三叶）----
  static const char* kBasicKeys[] = {
      "whiteBalance", "temperature", "tint",     "exposure", "contrast",
      "highlights",   "shadows",     "whites",   "blacks",   "clarity",
      "texture",      "dehaze",      "vibrance", "saturation",
  };
  for (const char* k : kBasicKeys) {
    emitScalar(params, k, std::string("basic.") + k, out);
  }
  // ---- calibration / detail / optics / colorGrade / parametric ----
  emitObjectLeaves(params.get("calibration"), "calibration.", out);
  emitObjectLeaves(params.get("detail"), "detail.", out);
  emitObjectLeaves(params.get("optics"), "optics.", out);
  emitObjectLeaves(params.get("colorGrade"), "colorGrade.", out);
  emitObjectLeaves(params.get("parametric"), "parametric.", out);
  // ---- geometry（crop 六叶 + perspective 七叶）----
  const json::JsonValue geo = params.get("geometry");
  if (geo.isObject()) {
    static const char* kGeoKeys[] = {"hasCrop", "cropTop",   "cropBottom",
                                     "cropLeft", "cropRight", "cropAngle"};
    for (const char* k : kGeoKeys) emitScalar(geo, k, std::string("geometry.") + k, out);
    emitObjectLeaves(geo.get("perspective"), "geometry.perspective.", out);
  }
  // ---- 曲线（数组原样；非数组忽略，算子端 <2 点回退恒等）----
  static const char* kCurveKeys[] = {"toneCurveRGB", "toneCurveR", "toneCurveG",
                                     "toneCurveB"};
  for (const char* k : kCurveKeys) {
    const json::JsonValue v = params.get(k);
    if (v.isArray()) out->push_back({std::string("curve.") + k, v});
  }
  // ---- HSL 8 带 × 3 通道 ----
  const json::JsonValue hsl = params.get("hsl");
  if (hsl.isObject()) {
    for (const auto& band : hsl.objectItems()) {
      if (!band.second.isObject()) continue;
      for (const char* ch : {"hue", "sat", "lum"}) {
        emitScalar(band.second, ch, "hsl." + band.first + "." + ch, out);
      }
    }
  }
  // ---- 蒙版（数组原样进 mask.apply；T2.2 实装蒙版数学）----
  const json::JsonValue masks = params.get("masks");
  if (masks.isArray()) out->push_back({"masks", masks});
  return true;
}

bool runEditPlan(const json::JsonValue& editParams, imaging::ImageBuf* buf,
                 RenderPlan* planOut, std::string* err) {
  // 算子注册：进程级一次性（线程安全、幂等——宿主进程可能已注册过，如测试/harness）。
  // 注册失败时回查算子表：已存在则视为已注册，否则永久记录，绝不静默重试。
  static std::once_flag once;
  static bool registered = false;
  static std::string registerErr;
  std::call_once(once, []() {
    std::string e;
    registered = registerBuiltinOps(&OpTable::instance(), &e);
    if (!registered) {
      e.clear();
      registered = OpTable::instance().find("pipeline.toneMap") != nullptr &&
                   OpTable::instance().find("basic.exposure") != nullptr;
      if (!registered) registerErr = e.empty() ? std::string("ops missing after failed register") : e;
    }
  });
  if (!registered) {
    if (err) *err = "registerBuiltinOps failed: " + registerErr;
    return false;
  }
  std::vector<std::pair<std::string, json::JsonValue>> kvs;
  if (!flattenEditParams(editParams, &kvs, err)) return false;
  RenderPlan tmp;
  RenderPlan* plan = planOut != nullptr ? planOut : &tmp;
  if (!compilePlan(0, kvs, plan, err)) return false;
  return executePlan(*plan, buf, err);
}

}  // namespace plan
}  // namespace lumina
