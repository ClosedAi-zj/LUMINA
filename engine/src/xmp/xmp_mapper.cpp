// XMP 映射实现：字段表驱动（crs: 标准字段 + lumina: 扩展）
#include "xmp/xmp_mapper.h"

#include <cmath>
#include <cstdio>
#include <cstring>

#include "core/logger.h"
#include "core/str_util.h"
#include "rpc/version.h"

namespace lumina {
namespace xmp {
namespace {

// 标量字段表：params 键 → XMP 前缀/字段名
struct ScalarField {
  const char* jsonKey;
  const char* prefix;
  const char* xmpName;
};

// §1 基础色调 + §2 色彩 + §3 几何光学：全部走 crs: 标准字段（映射表 §1–§3）
const ScalarField kScalarFields[] = {
    // §1
    {"whiteBalance", "crs", "WhiteBalance"},
    {"temperature", "crs", "Temperature"},
    {"tint", "crs", "Tint"},
    {"exposure", "crs", "Exposure2012"},
    {"contrast", "crs", "Contrast2012"},
    {"highlights", "crs", "Highlights2012"},
    {"shadows", "crs", "Shadows2012"},
    {"whites", "crs", "Whites2012"},
    {"blacks", "crs", "Blacks2012"},
    {"clarity", "crs", "Clarity2012"},
    {"texture", "crs", "Texture"},
    {"dehaze", "crs", "Dehaze"},
    {"vibrance", "crs", "Vibrance"},
    {"saturation", "crs", "Saturation"},
    {"parametricHighlights", "crs", "ParametricHighlights"},
    {"parametricLights", "crs", "ParametricLights"},
    {"parametricDarks", "crs", "ParametricDarks"},
    {"parametricShadows", "crs", "ParametricShadows"},
    {"parametricShadowSplit", "crs", "ParametricShadowSplit"},
    {"parametricLightSplit", "crs", "ParametricLightSplit"},
    {"parametricMidSplit", "crs", "ParametricMidSplit"},
    {"cameraProfile", "crs", "CameraProfile"},
    {"cameraProfileDigest", "crs", "CameraProfileDigest"},
    {"lookTable", "crs", "LookTable"},
    {"lookTableName", "crs", "LookTableName"},
    {"lookTableAmount", "crs", "LookTableAmount"},
    // §2 混色器 8 色 × hue/sat/lum = 24 字段（WP-A3：补齐 Orange 色带，crs: 标准字段名）
    {"hslRedHue", "crs", "HueAdjustmentRed"},
    {"hslOrangeHue", "crs", "HueAdjustmentOrange"},
    {"hslYellowHue", "crs", "HueAdjustmentYellow"},
    {"hslGreenHue", "crs", "HueAdjustmentGreen"},
    {"hslAquaHue", "crs", "HueAdjustmentAqua"},
    {"hslBlueHue", "crs", "HueAdjustmentBlue"},
    {"hslPurpleHue", "crs", "HueAdjustmentPurple"},
    {"hslMagentaHue", "crs", "HueAdjustmentMagenta"},
    {"hslRedSat", "crs", "SaturationAdjustmentRed"},
    {"hslOrangeSat", "crs", "SaturationAdjustmentOrange"},
    {"hslYellowSat", "crs", "SaturationAdjustmentYellow"},
    {"hslGreenSat", "crs", "SaturationAdjustmentGreen"},
    {"hslAquaSat", "crs", "SaturationAdjustmentAqua"},
    {"hslBlueSat", "crs", "SaturationAdjustmentBlue"},
    {"hslPurpleSat", "crs", "SaturationAdjustmentPurple"},
    {"hslMagentaSat", "crs", "SaturationAdjustmentMagenta"},
    {"hslRedLum", "crs", "LuminanceAdjustmentRed"},
    {"hslOrangeLum", "crs", "LuminanceAdjustmentOrange"},
    {"hslYellowLum", "crs", "LuminanceAdjustmentYellow"},
    {"hslGreenLum", "crs", "LuminanceAdjustmentGreen"},
    {"hslAquaLum", "crs", "LuminanceAdjustmentAqua"},
    {"hslBlueLum", "crs", "LuminanceAdjustmentBlue"},
    {"hslPurpleLum", "crs", "LuminanceAdjustmentPurple"},
    {"hslMagentaLum", "crs", "LuminanceAdjustmentMagenta"},
    // §2 分离色调 / 色彩分级
    {"shadowHue", "crs", "SplitToningShadowHue"},
    {"shadowSat", "crs", "SplitToningShadowSaturation"},
    {"midtoneHue", "crs", "ColorGradeMidtoneHue"},
    {"midtoneSat", "crs", "ColorGradeMidtoneSaturation"},
    {"highlightHue", "crs", "SplitToningHighlightHue"},
    {"highlightSat", "crs", "SplitToningHighlightSaturation"},
    {"gradeBlend", "crs", "ColorGradeBlend"},
    {"gradeBalance", "crs", "ColorGradeBalance"},
    // §2 校准
    {"calibrationShadowTint", "crs", "CalibrationShadowTint"},
    {"calibrationRedHue", "crs", "CalibrationRedHue"},
    {"calibrationRedSat", "crs", "CalibrationRedSat"},
    {"calibrationGreenHue", "crs", "CalibrationGreenHue"},
    {"calibrationGreenSat", "crs", "CalibrationGreenSat"},
    {"calibrationBlueHue", "crs", "CalibrationBlueHue"},
    {"calibrationBlueSat", "crs", "CalibrationBlueSat"},
    // §3 裁剪 / 透视
    {"cropTop", "crs", "CropTop"},
    {"cropBottom", "crs", "CropBottom"},
    {"cropLeft", "crs", "CropLeft"},
    {"cropRight", "crs", "CropRight"},
    {"cropAngle", "crs", "CropAngle"},
    {"perspectiveVertical", "crs", "PerspectiveVertical"},
    {"perspectiveHorizontal", "crs", "PerspectiveHorizontal"},
    {"perspectiveRotate", "crs", "PerspectiveRotate"},
    {"perspectiveScale", "crs", "PerspectiveScale"},
    {"perspectiveAspect", "crs", "PerspectiveAspect"},
    {"perspectiveX", "crs", "PerspectiveX"},
    {"perspectiveY", "crs", "PerspectiveY"},
    // §3 镜头 / 暗角 / 颗粒
    {"manualDistortion", "crs", "LensManualDistortionAmount"},
    {"vignetteAmount", "crs", "PostCropVignetteAmount"},
    {"vignetteMidpoint", "crs", "PostCropVignetteMidpoint"},
    {"vignetteFeather", "crs", "PostCropVignetteFeather"},
    {"grainAmount", "crs", "GrainAmount"},
    {"grainFrequency", "crs", "GrainFrequency"},
    {"grainSize", "crs", "GrainSize"},
    // §3 锐化 / 降噪
    {"sharpness", "crs", "Sharpness"},
    {"sharpenRadius", "crs", "SharpenRadius"},
    {"sharpenDetail", "crs", "SharpenDetail"},
    {"sharpenMasking", "crs", "SharpenMasking"},
    {"luminanceSmoothing", "crs", "LuminanceSmoothing"},
    {"colorNR", "crs", "ColorNoiseReduction"},
    {"colorNRDetail", "crs", "ColorNoiseReductionDetail"},
};

// 布尔字段（写 "True"/"False"，LR 习惯）
struct BoolField {
  const char* jsonKey;
  const char* prefix;
  const char* xmpName;
};
const BoolField kBoolFields[] = {
    {"hasCrop", "crs", "HasCrop"},
    {"lensProfileEnable", "crs", "LensProfileEnable"},
    {"uprightEnabled", "crs", "UprightTransform_Enabled"},
};

// lumina: 扩展字段（映射表 §5）：整块 JSON 序列化
struct JsonField {
  const char* jsonKey;
  const char* prefix;
  const char* xmpName;
};
const JsonField kJsonFields[] = {
    {"adjustmentStack", "lumina", "AdjustmentStack"},
    {"removals", "lumina", "Removals"},
    {"masks", "lumina", "Masks"},
    {"exposureRamp", "lumina", "ExposureRamp"},
    {"sequenceSync", "lumina", "SequenceSync"},
};

// ---- 嵌套参数形状 ↔ 扁平映射键 ----
// UI 专业模式按 7 个标签分组传参（hsl.red.hue / calibration.redSat / geometry.perspective.rotate …），
// 而映射表用的是扁平 CRS 键（hslRedHue / calibrationRedSat / perspectiveRotate）。
// 之前缺这层转换，导致引擎接受进内存、endSession 后一个字段都没写进 sidecar，
// 用户改完切走再回来全变默认值（专业模式 6 个标签全部受影响）。
namespace {

struct NestedMap {
  const char* objectKey;
  const char* subKey;
  const char* flatKey;
};

const NestedMap kNestedFields[] = {
    // 色彩分级（映射表里是 shadowHue/gradeBlend 这组扁平名）
    {"colorGrade", "shadowHue", "shadowHue"},
    {"colorGrade", "shadowSat", "shadowSat"},
    {"colorGrade", "midtoneHue", "midtoneHue"},
    {"colorGrade", "midtoneSat", "midtoneSat"},
    {"colorGrade", "highlightHue", "highlightHue"},
    {"colorGrade", "highlightSat", "highlightSat"},
    {"colorGrade", "blend", "gradeBlend"},
    {"colorGrade", "balance", "gradeBalance"},
    // 校准
    {"calibration", "shadowTint", "calibrationShadowTint"},
    {"calibration", "redHue", "calibrationRedHue"},
    {"calibration", "redSat", "calibrationRedSat"},
    {"calibration", "greenHue", "calibrationGreenHue"},
    {"calibration", "greenSat", "calibrationGreenSat"},
    {"calibration", "blueHue", "calibrationBlueHue"},
    {"calibration", "blueSat", "calibrationBlueSat"},
    // 细节（与扁平键同名）
    {"detail", "sharpness", "sharpness"},
    {"detail", "sharpenRadius", "sharpenRadius"},
    {"detail", "sharpenDetail", "sharpenDetail"},
    {"detail", "sharpenMasking", "sharpenMasking"},
    {"detail", "luminanceSmoothing", "luminanceSmoothing"},
    {"detail", "colorNR", "colorNR"},
    {"detail", "colorNRDetail", "colorNRDetail"},
    // 镜头 / 效果（与扁平键同名）
    {"optics", "lensProfileEnable", "lensProfileEnable"},
    {"optics", "manualDistortion", "manualDistortion"},
    {"optics", "vignetteAmount", "vignetteAmount"},
    {"optics", "vignetteMidpoint", "vignetteMidpoint"},
    {"optics", "vignetteFeather", "vignetteFeather"},
    {"optics", "grainAmount", "grainAmount"},
    {"optics", "grainFrequency", "grainFrequency"},
    {"optics", "grainSize", "grainSize"},
    // 几何
    {"geometry", "hasCrop", "hasCrop"},
};

const char* kHslBands[][2] = {{"red", "Red"},     {"orange", "Orange"}, {"yellow", "Yellow"},
                              {"green", "Green"}, {"aqua", "Aqua"},     {"blue", "Blue"},
                              {"purple", "Purple"}, {"magenta", "Magenta"}};
const char* kHslKeys[][2] = {{"hue", "Hue"}, {"sat", "Sat"}, {"lum", "Lum"}};
const char* kPerspectiveKeys[][2] = {{"vertical", "perspectiveVertical"},
                                     {"horizontal", "perspectiveHorizontal"},
                                     {"rotate", "perspectiveRotate"},
                                     {"scale", "perspectiveScale"},
                                     {"aspect", "perspectiveAspect"},
                                     {"x", "perspectiveX"},
                                     {"y", "perspectiveY"}};

// 嵌套 → 扁平。顶层标量原样带过来，嵌套对象展开成映射表的扁平键。
json::JsonValue flattenParams(const json::JsonValue& params) {
  json::JsonValue flat = json::JsonValue::objectValue();
  if (!params.isObject()) return flat;
  for (const auto& item : params.objectItems()) {
    if (item.second.isObject()) continue;  // 嵌套对象下面单独展开
    flat.setItem(item.first, item.second);
  }
  for (const NestedMap& m : kNestedFields) {
    const json::JsonValue& obj = params.get(m.objectKey);
    if (!obj.isObject()) continue;
    const json::JsonValue& v = obj.get(m.subKey);
    if (!v.isNull()) flat.setItem(m.flatKey, v);
  }
  const json::JsonValue& hsl = params.get("hsl");
  if (hsl.isObject()) {
    for (const auto& band : kHslBands) {
      const json::JsonValue& b = hsl.get(band[0]);
      if (!b.isObject()) continue;
      for (const auto& key : kHslKeys) {
        const json::JsonValue& v = b.get(key[0]);
        if (v.isNull()) continue;
        flat.setItem("hsl" + std::string(band[1]) + key[1], v);
      }
    }
  }
  const json::JsonValue& persp = params.get("geometry").get("perspective");
  if (persp.isObject()) {
    for (const auto& key : kPerspectiveKeys) {
      const json::JsonValue& v = persp.get(key[0]);
      if (!v.isNull()) flat.setItem(key[1], v);
    }
  }
  return flat;
}

// 扁平 → 嵌套。读取旧 sidecar 时让 UI 拿到的形状与它写出去的一致。
void unflattenInto(const json::JsonValue& flat, json::JsonValue* out) {
  if (!out || !out->isObject()) return;
  for (const NestedMap& m : kNestedFields) {
    if (!flat.contains(m.flatKey)) continue;
    json::JsonValue obj = out->get(m.objectKey);
    if (!obj.isObject()) obj = json::JsonValue::objectValue();
    obj.setItem(m.subKey, flat.get(m.flatKey));
    out->setItem(m.objectKey, obj);
  }
  json::JsonValue hsl = json::JsonValue::objectValue();
  for (const auto& band : kHslBands) {
    json::JsonValue b = json::JsonValue::objectValue();
    bool any = false;
    for (const auto& key : kHslKeys) {
      std::string flatKey = "hsl" + std::string(band[1]) + key[1];
      if (!flat.contains(flatKey)) continue;
      b.setItem(key[0], flat.get(flatKey));
      any = true;
    }
    if (any) hsl.setItem(band[0], b);
  }
  if (hsl.objectItems().size() > 0) out->setItem("hsl", hsl);
  json::JsonValue persp = json::JsonValue::objectValue();
  bool anyPersp = false;
  for (const auto& key : kPerspectiveKeys) {
    if (!flat.contains(key[1])) continue;
    persp.setItem(key[0], flat.get(key[1]));
    anyPersp = true;
  }
  if (anyPersp) {
    json::JsonValue geo = out->get("geometry");
    if (!geo.isObject()) geo = json::JsonValue::objectValue();
    geo.setItem("perspective", persp);
    out->setItem("geometry", geo);
  }
}

}  // namespace

std::string formatNumber(double v) {
  if (std::isnan(v) || std::isinf(v)) return "0";
  char buf[48];
  double r = std::floor(v + 0.5);
  if (std::fabs(v - r) < 1e-6) {
    snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(r));
    return std::string(buf);
  }
  snprintf(buf, sizeof(buf), "%.3f", v);
  return std::string(buf);
}

std::string serializeCurve(const json::JsonValue& curve) {
  // {x,y}[] → "x, y x, y"
  std::string out;
  if (!curve.isArray()) return out;
  bool first = true;
  for (const auto& point : curve.arrayItems()) {
    double x = point.get("x").asDouble();
    double y = point.get("y").asDouble();
    if (!first) out += " ";
    out += formatNumber(x) + ", " + formatNumber(y);
    first = false;
  }
  return out;
}

json::JsonValue parseCurve(const std::string& text) {
  json::JsonValue out = json::JsonValue::arrayValue();
  std::string cur;
  std::vector<std::string> tokens;
  for (char c : text) {
    if (c == ' ' || c == '\t' || c == '\n') {
      if (!cur.empty()) tokens.push_back(cur);
      cur.clear();
    } else {
      cur.push_back(c);
    }
  }
  if (!cur.empty()) tokens.push_back(cur);
  for (const std::string& t : tokens) {
    size_t comma = t.find(',');
    if (comma == std::string::npos) continue;
    json::JsonValue p = json::JsonValue::objectValue();
    p.setItem("x", atof(t.substr(0, comma).c_str()));
    p.setItem("y", atof(t.substr(comma + 1).c_str()));
    out.addItem(p);
  }
  return out;
}

// 嵌套键读取辅助（如 hsl.red.hue）
void setNumber(json::JsonValue* params, const std::string& key, double value) {
  params->setItem(key, value);
}

}  // namespace

json::JsonValue XmpMapper::defaultParams() {
  json::JsonValue p = json::JsonValue::objectValue();
  p.setItem("processVersion", kMaxProcessVersion);
  p.setItem("whiteBalance", "asShot");
  p.setItem("exposure", 0.0);
  p.setItem("contrast", 0.0);
  p.setItem("highlights", 0.0);
  p.setItem("shadows", 0.0);
  p.setItem("whites", 0.0);
  p.setItem("blacks", 0.0);
  p.setItem("clarity", 0.0);
  p.setItem("texture", 0.0);
  p.setItem("dehaze", 0.0);
  p.setItem("vibrance", 0.0);
  p.setItem("saturation", 0.0);
  p.setItem("masks", json::JsonValue::arrayValue());
  return p;
}

void XmpMapper::readFromDoc(const XmpDocument& doc, json::JsonValue* out,
                            std::vector<std::string>* warnings) {
  *out = defaultParams();
  for (const ScalarField& f : kScalarFields) {
    if (doc.has(f.prefix, f.xmpName)) {
      std::string v = trim(doc.get(f.prefix, f.xmpName));
      if (!v.empty()) {
        double d = 0.0;
        if (sscanf(v.c_str(), "%lf", &d) == 1) out->setItem(f.jsonKey, d);
        else out->setItem(f.jsonKey, v);
      }
    }
  }
  for (const BoolField& f : kBoolFields) {
    if (doc.has(f.prefix, f.xmpName)) {
      out->setItem(f.jsonKey, doc.getBool(f.prefix, f.xmpName, false));
    }
  }
  // 色调曲线
  const struct {
    const char* jsonKey;
    const char* xmpName;
  } curves[] = {{"toneCurveRGB", "ToneCurvePV2012"},
                {"toneCurveR", "ToneCurvePV2012Red"},
                {"toneCurveG", "ToneCurvePV2012Green"},
                {"toneCurveB", "ToneCurvePV2012Blue"}};
  for (const auto& c : curves) {
    if (doc.has("crs", c.xmpName)) {
      out->setItem(c.jsonKey, parseCurve(doc.get("crs", c.xmpName)));
    }
  }
  // lumina: 扩展（整块 JSON）
  for (const JsonField& f : kJsonFields) {
    if (doc.has(f.prefix, f.xmpName)) {
      std::string text = doc.get(f.prefix, f.xmpName);
      json::JsonValue parsed;
      std::string err;
      if (json::JsonValue::parse(text, &parsed, &err)) {
        out->setItem(f.jsonKey, parsed);
      } else {
        warnings->push_back(std::string("lumina:") + f.xmpName + " 解析失败: " + err);
      }
    }
  }
  // lumina: 单值字段
  if (doc.has("lumina", "VirtualCopyOf")) out->setItem("virtualCopyOf", doc.get("lumina", "VirtualCopyOf"));
  if (doc.has("lumina", "CullStatus")) out->setItem("cullStatus", doc.get("lumina", "CullStatus"));
  if (doc.has("lumina", "SchemaVersion")) {
    out->setItem("schemaVersion", doc.getInt("lumina", "SchemaVersion", 1));
  }
  // lrt: Holy Grail 关键帧（只读优先，映射表 §6）
  if (doc.has("lrt", "Keyframes")) {
    out->setItem("lrtKeyframes", doc.get("lrt", "Keyframes"));
  }
  if (doc.has("crs", "ProcessVersion")) {
    out->setItem("processVersion", doc.get("crs", "ProcessVersion"));
  } else {
    out->setItem("processVersion", kMaxProcessVersion);
  }
  // 把扁平 CRS 键组装回 UI 使用的嵌套形状（hsl / colorGrade / calibration / detail /
  // optics / geometry.perspective），保证"读回来的形状 == 写出去的形状"。
  unflattenInto(*out, out);
}

bool XmpMapper::processVersionTooNew(const XmpDocument& doc, std::string* found) {
  if (!doc.has("crs", "ProcessVersion")) return false;
  std::string pv = trim(doc.get("crs", "ProcessVersion"));
  if (pv.empty()) return false;
  *found = pv;
  // 主.次 比较：主版本更大即"高于引擎认知"
  int major = 0;
  if (sscanf(pv.c_str(), "%d", &major) != 1) return false;
  int knownMajor = 0;
  if (sscanf(kMaxProcessVersion, "%d", &knownMajor) != 1) return false;
  return major > knownMajor;
}

void XmpMapper::writeToDoc(const json::JsonValue& params, XmpDocument* doc) {
  if (!params.isObject()) return;
  // 嵌套形状 → 扁平键；后续一律读 flat
  const json::JsonValue flat = flattenParams(params);
  for (const ScalarField& f : kScalarFields) {
    const json::JsonValue& v = flat.get(f.jsonKey);
    if (v.isNull()) continue;
    if (v.isNumber()) {
      doc->setText(f.prefix, f.xmpName, formatNumber(v.asDouble()));
    } else if (v.isString()) {
      doc->setText(f.prefix, f.xmpName, v.stringRef());
    } else if (v.isBool()) {
      doc->setText(f.prefix, f.xmpName, v.asBool() ? "True" : "False");
    }
  }
  for (const BoolField& f : kBoolFields) {
    const json::JsonValue& v = flat.get(f.jsonKey);
    if (v.isNull()) continue;
    doc->setText(f.prefix, f.xmpName, v.asBool() ? "True" : "False");
  }
  const struct {
    const char* jsonKey;
    const char* xmpName;
  } curves[] = {{"toneCurveRGB", "ToneCurvePV2012"},
                {"toneCurveR", "ToneCurvePV2012Red"},
                {"toneCurveG", "ToneCurvePV2012Green"},
                {"toneCurveB", "ToneCurvePV2012Blue"}};
  for (const auto& c : curves) {
    const json::JsonValue& v = flat.get(c.jsonKey);
    if (!v.isArray() || v.size() == 0) continue;
    std::string text = serializeCurve(v);
    if (!text.empty()) doc->setText("crs", c.xmpName, text);
  }
  for (const JsonField& f : kJsonFields) {
    const json::JsonValue& v = flat.get(f.jsonKey);
    if (v.isNull()) continue;
    doc->setRaw(f.prefix, f.xmpName, v.serialize());
  }
  if (params.contains("virtualCopyOf")) {
    doc->setText("lumina", "VirtualCopyOf", flat.get("virtualCopyOf").asString());
  }
  if (params.contains("cullStatus")) {
    doc->setText("lumina", "CullStatus", flat.get("cullStatus").asString());
  }
  // SchemaVersion 必须始终存在（映射表 §7）
  doc->setText("lumina", "SchemaVersion", std::to_string(kXmpSchemaVersion));
  doc->setText("crs", "ProcessVersion", kMaxProcessVersion);
}

}  // namespace xmp
}  // namespace lumina
