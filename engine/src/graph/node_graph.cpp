#include "graph/node_graph.h"

#include <set>

namespace lumina {
namespace graph {

namespace {

// 默认值取 §4 表缺省口径（与 crs: 同值直写，无换算）
json::JsonValue num(double v) { return json::JsonValue::number(v); }
json::JsonValue strv(const char* s) { return json::JsonValue::str(s); }

}  // namespace

const char* nodeTypeName(NodeType t) {
  switch (t) {
    case NodeType::Basic: return "basic";
    case NodeType::Curve: return "curve";
    case NodeType::Color: return "color";
    case NodeType::Detail: return "detail";
    case NodeType::Lens: return "lens";
    case NodeType::Geometry: return "geometry";
    case NodeType::Effects: return "effects";
    case NodeType::Calibration: return "calibration";
    default: return "?";
  }
}

bool nodeTypeFromString(const std::string& s, NodeType* out) {
  for (int i = 0; i < static_cast<int>(NodeType::kCount); ++i) {
    if (s == nodeTypeName(static_cast<NodeType>(i))) {
      if (out) *out = static_cast<NodeType>(i);
      return true;
    }
  }
  return false;
}

bool isBlendModeValid(const std::string& m) {
  return m == "normal" || m == "multiply" || m == "screen" ||
         m == "overlay" || m == "softLight";
}

// T2.5：MaskCombine 字符串映射（sidecar JSON 口径，缺省 replace）
const char* maskCombineName(MaskCombine c) {
  switch (c) {
    case MaskCombine::Intersect: return "intersect";
    case MaskCombine::Subtract: return "subtract";
    case MaskCombine::Replace: return "replace";
    default: return "replace";
  }
}

bool maskCombineFromString(const std::string& s, MaskCombine* out) {
  if (s == "replace") {
    if (out) *out = MaskCombine::Replace;
    return true;
  }
  if (s == "intersect") {
    if (out) *out = MaskCombine::Intersect;
    return true;
  }
  if (s == "subtract") {
    if (out) *out = MaskCombine::Subtract;
    return true;
  }
  return false;
}

json::JsonValue defaultParamsFor(NodeType t) {
  json::JsonValue p = json::JsonValue::objectValue();
  switch (t) {
    case NodeType::Basic:
      p.setItem("whiteBalance", strv("asShot"));
      p.setItem("temperature", num(0.0));
      p.setItem("tint", num(0.0));
      p.setItem("exposure", num(0.0));
      p.setItem("contrast", num(0.0));
      p.setItem("highlights", num(0.0));
      p.setItem("shadows", num(0.0));
      p.setItem("whites", num(0.0));
      p.setItem("blacks", num(0.0));
      p.setItem("clarity", num(0.0));
      p.setItem("texture", num(0.0));
      p.setItem("dehaze", num(0.0));
      p.setItem("vibrance", num(0.0));
      p.setItem("saturation", num(0.0));
      break;
    case NodeType::Curve:
      p.setItem("toneCurveRGB", json::JsonValue::nullValue());
      p.setItem("toneCurveR", json::JsonValue::nullValue());
      p.setItem("toneCurveG", json::JsonValue::nullValue());
      p.setItem("toneCurveB", json::JsonValue::nullValue());
      p.setItem("parametric", json::JsonValue::nullValue());
      break;
    case NodeType::Color: {
      static const char* kHues[8] = {"red",    "orange", "yellow", "green",
                                     "aqua",   "blue",   "purple", "magenta"};
      for (const char* band : kHues) {
        p.setItem(std::string("hsl.") + band + ".hue", num(0.0));
        p.setItem(std::string("hsl.") + band + ".sat", num(0.0));
        p.setItem(std::string("hsl.") + band + ".lum", num(0.0));
      }
      p.setItem("colorGrade.shadowHue", num(0.0));
      p.setItem("colorGrade.shadowSat", num(0.0));
      p.setItem("colorGrade.midtoneHue", num(0.0));
      p.setItem("colorGrade.midtoneSat", num(0.0));
      p.setItem("colorGrade.highlightHue", num(0.0));
      p.setItem("colorGrade.highlightSat", num(0.0));
      p.setItem("colorGrade.blend", num(50.0));
      p.setItem("colorGrade.balance", num(0.0));
      break;
    }
    case NodeType::Detail:
      p.setItem("sharpness", num(0.0));
      p.setItem("sharpenRadius", num(1.0));
      p.setItem("sharpenDetail", num(25.0));
      p.setItem("sharpenMasking", num(0.0));
      p.setItem("colorNR", num(0.0));
      p.setItem("colorNRDetail", num(50.0));
      p.setItem("luminanceSmoothing", num(0.0));
      break;
    case NodeType::Lens:
      p.setItem("lensProfileEnable", num(0.0));
      p.setItem("manualDistortion", num(0.0));
      break;
    case NodeType::Geometry:
      p.setItem("hasCrop", num(0.0));
      p.setItem("cropLeft", num(0.0));
      p.setItem("cropTop", num(0.0));
      p.setItem("cropRight", num(1.0));
      p.setItem("cropBottom", num(1.0));
      p.setItem("cropAngle", num(0.0));
      p.setItem("perspective.x", num(0.0));
      p.setItem("perspective.y", num(0.0));
      p.setItem("perspective.horizontal", num(0.0));
      p.setItem("perspective.vertical", num(0.0));
      p.setItem("perspective.rotate", num(0.0));
      p.setItem("perspective.scale", num(1.0));  // 倍数语义，1=恒等（与 geometry.cpp/口径一致）
      p.setItem("perspective.aspect", num(0.0));
      break;
    case NodeType::Effects:
      p.setItem("vignetteAmount", num(0.0));
      p.setItem("vignetteMidpoint", num(50.0));
      p.setItem("vignetteFeather", num(50.0));
      p.setItem("grainAmount", num(0.0));
      p.setItem("grainSize", num(0.0));
      p.setItem("grainFrequency", num(0.0));
      break;
    case NodeType::Calibration:
      p.setItem("shadowTint", num(0.0));
      p.setItem("redHue", num(0.0));
      p.setItem("redSat", num(0.0));
      p.setItem("greenHue", num(0.0));
      p.setItem("greenSat", num(0.0));
      p.setItem("blueHue", num(0.0));
      p.setItem("blueSat", num(0.0));
      break;
    default:
      break;
  }
  return p;
}

NodeGraph defaultGraph() {
  NodeGraph g;
  g.schemaVersion = 2;
  static const NodeType kTypes[8] = {
      NodeType::Basic, NodeType::Curve,     NodeType::Color,
      NodeType::Detail, NodeType::Lens,     NodeType::Geometry,
      NodeType::Effects, NodeType::Calibration};
  for (int i = 0; i < 8; ++i) {
    GraphNode n;
    n.id = "n" + std::to_string(i + 1);
    n.type = kTypes[i];
    n.order = 0;
    n.enabled = true;
    n.opacity = 100;
    n.blendMode = "normal";
    n.hasMask = false;
    n.params = defaultParamsFor(kTypes[i]);
    g.nodes.push_back(std::move(n));
  }
  return g;
}

}  // namespace graph
}  // namespace lumina
