// T2.6 蒙版数学唯一权威实现（N2）。公式来源：架构 §3.1.1/§3.1.3/§3.1.4/§1.4；
// 画笔场逐字镜像 app/src/modules/develop/webgl/maskField.ts（S7 将与 TS 常量交叉断言）。
// 本文件是蒙版因子与布尔 fold 的唯一权威：GPU mask pass（T2.7）与 CPU oracle
// （applyMasksOracle，M25）必须逐字镜像这里的公式，禁止第二实现。
#include "plan/ops/mask_field.h"

#include <algorithm>
#include <cmath>

#include "plan/ops/ops.h"  // smooth01 / clamp01 / rgbToHsl（与全局算子共享同一实现）

namespace lumina {
namespace plan {

const char* maskTypeName(MaskType t) {
  switch (t) {
    case MaskType::Linear: return "linear";
    case MaskType::Radial: return "radial";
    case MaskType::Brush: return "brush";
    case MaskType::LuminanceRange: return "luminanceRange";
    case MaskType::ColorRange: return "colorRange";
    case MaskType::Depth: return "depth";
    case MaskType::AiSky: return "aiSky";
    case MaskType::AiSubject: return "aiSubject";
    case MaskType::AiBackground: return "aiBackground";
    case MaskType::kUnknown: break;
  }
  return "unknown";
}

bool maskTypeFromString(const std::string& s, MaskType* out) {
  if (s == "linear") { *out = MaskType::Linear; return true; }
  if (s == "radial") { *out = MaskType::Radial; return true; }
  if (s == "brush") { *out = MaskType::Brush; return true; }
  if (s == "luminanceRange") { *out = MaskType::LuminanceRange; return true; }
  if (s == "colorRange") { *out = MaskType::ColorRange; return true; }
  if (s == "depth") { *out = MaskType::Depth; return true; }
  if (s == "aiSky") { *out = MaskType::AiSky; return true; }
  if (s == "aiSubject") { *out = MaskType::AiSubject; return true; }
  if (s == "aiBackground") { *out = MaskType::AiBackground; return true; }
  return false;
}

namespace {

// geometry/range 对象里的数值读取（缺失/非数给缺省）
double geoNum(const json::JsonValue& o, const char* key, double def) {
  return o.get(key).asDouble(def);
}

// 裁后归一化 uv（§1.4：u=(x+0.5)/w，v 向下）
inline double uvU(int x, int w) { return (static_cast<double>(x) + 0.5) / static_cast<double>(w); }
inline double uvV(int y, int h) { return (static_cast<double>(y) + 0.5) / static_cast<double>(h); }

// ---- linear：梯度投影，t=clamp01 线性坡（(x1,y1)=0 → (x2,y2)=1）----
double linearFactorRaw(const json::JsonValue& g, double u, double v) {
  const double x1 = geoNum(g, "x1", 0.0);
  const double y1 = geoNum(g, "y1", 0.0);
  const double x2 = geoNum(g, "x2", 1.0);
  const double y2 = geoNum(g, "y2", 1.0);
  const double dx = x2 - x1;
  const double dy = y2 - y1;
  const double len2 = dx * dx + dy * dy;
  if (len2 <= 1e-12) return 0.0;  // 退化（零长度梯度 = 未画出梯度）→ 无效果
  const double t = ((u - x1) * dx + (v - y1) * dy) / len2;
  return clamp01(t);
}

// ---- radial：椭圆距离 + feather 平滑衰减（§1.4 口径）----
// d = sqrt(dx² + (dy/ratio)²)；raw = 1 - smooth01(d, radius*(1-feather), radius)。
// feather 缺省 0.45（衰减起点 0.55r，与 v1.0 GPU 径向蒙版 smoothstep5(0.55r, r, d) 一致，
// 保证存量观感不变）；ratio ≤ 0 视为 1；feather 截断到 [0,1]。
double radialFactorRaw(const json::JsonValue& g, double u, double v) {
  const double cx = geoNum(g, "cx", 0.5);
  const double cy = geoNum(g, "cy", 0.5);
  const double r = geoNum(g, "radius", 0.4);
  double ratio = geoNum(g, "ratio", 1.0);
  if (ratio <= 1e-9) ratio = 1.0;
  double feather = geoNum(g, "feather", 0.45);
  if (feather < 0.0) feather = 0.0;
  if (feather > 1.0) feather = 1.0;
  const double dx = u - cx;
  const double dy = (v - cy) / ratio;
  const double d = std::sqrt(dx * dx + dy * dy);
  return 1.0 - smooth01(d, r * (1.0 - feather), r);
}

// ---- luminanceRange：low/high 窗口 + smooth 过渡带（显示域 L）----
// raw = smooth01(L, low-s, low) * (1 - smooth01(L, high, high+s))，s = smooth ∈ [0,1]。
// smooth 缺省 0.1（协议 smooth? 可选；口径登记见 T2.6 交付报告）。
double lumRangeFactorRaw(const json::JsonValue& rg, double L) {
  const double low = geoNum(rg, "low", 0.0);
  const double high = geoNum(rg, "high", 1.0);
  const double s = clamp01(geoNum(rg, "smooth", 0.1));
  return smooth01(L, low - s, low) * (1.0 - smooth01(L, high, high + s));
}

// ---- colorRange：色相最短弧窗口 × 饱和度窗口（显示域 HSL）----
// 色相窗口：half = hueWidth/2，inner = half*(1-sm)，fHue = 1 - smooth01(|dh|, inner, half)；
// 饱和度窗口：fSat = smooth01(s, satLow, satLow+sm) * (1 - smooth01(s, satHigh, satHigh+sm))。
// hue/hueCenter ∈ [0,1)（rgbToHsl 口径），色相环绕按最短弧（§1.4）。
double colorRangeFactorRaw(const json::JsonValue& rg, double r, double g, double b) {
  const double hueCenter = geoNum(rg, "hueCenter", 0.0);
  const double hueWidth = geoNum(rg, "hueWidth", 0.1);
  const double satLow = geoNum(rg, "satLow", 0.0);
  const double satHigh = geoNum(rg, "satHigh", 1.0);
  const double s = clamp01(geoNum(rg, "smooth", 0.1));
  double h = 0.0, sat = 0.0, l = 0.0;
  rgbToHsl(clamp01(r), clamp01(g), clamp01(b), &h, &sat, &l);
  // 最短弧：把色差折进 [-0.5, 0.5)
  double dh = h - hueCenter;
  dh -= std::floor(dh + 0.5);
  const double half = std::max(0.0, hueWidth) * 0.5;
  const double fHue = 1.0 - smooth01(std::fabs(dh), half * (1.0 - s), half);
  const double fSat =
      smooth01(sat, satLow, satLow + s) * (1.0 - smooth01(sat, satHigh, satHigh + s));
  return fHue * fSat;
}

// ---- 画笔场（镜像 maskField.ts）----
struct StrokeSeg {
  double x1 = 0.0, y1 = 0.0, x2 = 0.0, y2 = 0.0;
  double radius = 0.0, flow = 0.0;
  bool erase = false;
};

// 相邻印记连成线段；erase 标志翻转处断开（画与擦是两条折线，擦除走乘法衰减）
std::vector<StrokeSeg> toSegments(const json::JsonValue& strokes) {
  std::vector<StrokeSeg> segs;
  if (!strokes.isArray()) return segs;
  segs.reserve(strokes.size());
  for (size_t i = 0; i < strokes.size(); ++i) {
    const json::JsonValue& cur = strokes.at(i);
    StrokeSeg s;
    const double cx = cur.get("x").asDouble(0.0);
    const double cy = cur.get("y").asDouble(0.0);
    s.radius = cur.get("radius").asDouble(0.0);
    s.flow = cur.get("flow").asDouble(100.0);
    s.erase = cur.get("erase").asBool(false);
    if (i > 0) {
      const json::JsonValue& prev = strokes.at(i - 1);
      const bool connect =
          prev.get("erase").asBool(false) == s.erase;  // 同 erase 标志才连线
      s.x1 = connect ? prev.get("x").asDouble(0.0) : cx;
      s.y1 = connect ? prev.get("y").asDouble(0.0) : cy;
    } else {
      s.x1 = cx;
      s.y1 = cy;
    }
    s.x2 = cx;
    s.y2 = cy;
    segs.push_back(s);
  }
  return segs;
}

// 点到线段距离（归一化坐标空间，与 maskField.ts segDistance 同式）
double segDistance(double px, double py, const StrokeSeg& s) {
  const double dx = s.x2 - s.x1;
  const double dy = s.y2 - s.y1;
  const double len2 = dx * dx + dy * dy;
  double t = 0.0;
  if (len2 > 1e-12) {
    t = ((px - s.x1) * dx + (py - s.y1) * dy) / len2;
    if (t < 0.0) t = 0.0;
    if (t > 1.0) t = 1.0;
  }
  const double ex = px - (s.x1 + t * dx);
  const double ey = py - (s.y1 + t * dy);
  return std::sqrt(ex * ex + ey * ey);
}

}  // namespace

void buildBrushField256(const json::JsonValue& strokes,
                        float out[kBrushFieldCell * kBrushFieldCell]) {
  constexpr int kCell = kBrushFieldCell;
  for (int i = 0; i < kCell * kCell; ++i) out[i] = 0.0f;
  if (!strokes.isArray() || strokes.empty()) return;
  const std::vector<StrokeSeg> segs = toSegments(strokes);
  // 每段包围盒 + 半径外扩：像素级裁剪（镜像 maskField.ts buildBrushField 的 boxes）
  struct Box {
    double minX = 0.0, maxX = 0.0, minY = 0.0, maxY = 0.0;
    const StrokeSeg* seg = nullptr;
  };
  std::vector<Box> boxes;
  boxes.reserve(segs.size());
  for (const StrokeSeg& s : segs) {
    Box b;
    b.minX = std::min(s.x1, s.x2) - s.radius;
    b.maxX = std::max(s.x1, s.x2) + s.radius;
    b.minY = std::min(s.y1, s.y2) - s.radius;
    b.maxY = std::max(s.y1, s.y2) + s.radius;
    b.seg = &s;
    boxes.push_back(b);
  }
  for (int py = 0; py < kCell; ++py) {
    const double ny = (static_cast<double>(py) + 0.5) / kCell;  // 画面纵向比例（顶=0）
    for (int px = 0; px < kCell; ++px) {
      const double nx = (static_cast<double>(px) + 0.5) / kCell;
      double factor = 0.0;
      for (const Box& b : boxes) {
        if (nx < b.minX || nx > b.maxX || ny < b.minY || ny > b.maxY) continue;
        const StrokeSeg& s = *b.seg;
        const double d = segDistance(nx, ny, s);
        // w = (1 - smoothstep5(r/2, r, d)) * (flow/100)（与 maskField.ts:96 同式）
        const double w = (1.0 - smooth01(d, s.radius * 0.5, s.radius)) * (s.flow / 100.0);
        if (w <= 0.0) continue;
        if (s.erase) {
          factor *= 1.0 - w;  // 擦除组：乘法衰减
        } else if (w > factor) {
          factor = w;  // 画笔组：取 max（叠加取深）
        }
      }
      // 8bit 量化：与预览 LUMINANCE 纹理（maskField.ts buildFieldAtlas 的字节上传）
      // 严格同值，保证引擎导出与 GPU 预览的场采样逐字节一致。
      double q = std::floor(factor * 255.0 + 0.5) / 255.0;
      if (q < 0.0) q = 0.0;
      if (q > 1.0) q = 1.0;
      out[static_cast<size_t>(py) * kCell + px] = static_cast<float>(q);
    }
  }
}

double sampleBrushField256(const float field[kBrushFieldCell * kBrushFieldCell],
                           double u, double v) {
  constexpr int kCell = kBrushFieldCell;
  // 镜像 maskField.ts sampleAtlas：fx/fy 截到 [0, CELL-1]，x1/y1 截到 CELL-1
  double fx = u * kCell - 0.5;
  double fy = v * kCell - 0.5;
  if (fx < 0.0) fx = 0.0;
  if (fx > kCell - 1) fx = kCell - 1.0;
  if (fy < 0.0) fy = 0.0;
  if (fy > kCell - 1) fy = kCell - 1.0;
  const int x0 = static_cast<int>(std::floor(fx));
  const int y0 = static_cast<int>(std::floor(fy));
  const double tx = fx - x0;
  const double ty = fy - y0;
  const int x1 = std::min(kCell - 1, x0 + 1);
  const int y1 = std::min(kCell - 1, y0 + 1);
  const double a = field[static_cast<size_t>(y0) * kCell + x0];
  const double b = field[static_cast<size_t>(y0) * kCell + x1];
  const double c = field[static_cast<size_t>(y1) * kCell + x0];
  const double d = field[static_cast<size_t>(y1) * kCell + x1];
  return (a * (1.0 - tx) + b * tx) * (1.0 - ty) + (c * (1.0 - tx) + d * tx) * ty;
}

bool parseMaskSpec(const json::JsonValue& j, MaskSpec* out, std::string* err) {
  if (out == nullptr) {
    if (err) *err = "parseMaskSpec: null out";
    return false;
  }
  *out = MaskSpec();
  if (!j.isObject()) {
    if (err) *err = "parseMaskSpec: mask 项必须是对象";
    return false;
  }
  out->id = j.get("id").asString();
  const std::string typeStr = j.get("type").asString();
  if (!maskTypeFromString(typeStr, &out->type)) {
    out->type = MaskType::kUnknown;  // 求值时显式报错（A5），此处只登记
  }
  out->enabled = j.get("enabled").asBool(true);
  // combine：缺省 replace；非法显式失败（与 graph/serialize.cpp gmaskFromJson 同口径）
  const std::string combineStr = j.get("combine").asString("replace");
  if (!graph::maskCombineFromString(combineStr, &out->combine)) {
    if (err) *err = "mask '" + out->id + "': 未知 combine '" + combineStr + "'";
    return false;
  }
  int64_t opacity = j.get("opacity").asInt(100);
  if (opacity < 0) opacity = 0;
  if (opacity > 100) opacity = 100;
  out->opacity = static_cast<int>(opacity);
  int64_t amount = j.get("amount").asInt(100);
  if (amount < -100) amount = -100;
  if (amount > 100) amount = 100;
  out->amount = static_cast<int>(amount);
  out->geometry = j.get("geometry");
  out->range = j.get("range");
  out->strokes = j.get("strokes");
  out->local = j.get("local");
  return true;
}

namespace {

// 单蒙版 raw 因子平面（无 fold；尺寸 = buf）。不支持类型 → false + err（A5）。
bool evalRawPlane(const MaskSpec& m, const imaging::ImageBuf& buf, MaskPlane* out,
                  std::string* err) {
  const int w = buf.w;
  const int h = buf.h;
  out->w = w;
  out->h = h;
  out->f.assign(static_cast<size_t>(w) * h, 0.0f);
  switch (m.type) {
    case MaskType::Linear:
      for (int y = 0; y < h; ++y) {
        const double v = uvV(y, h);
        for (int x = 0; x < w; ++x) {
          out->f[static_cast<size_t>(y) * w + x] =
              static_cast<float>(linearFactorRaw(m.geometry, uvU(x, w), v));
        }
      }
      return true;
    case MaskType::Radial:
      for (int y = 0; y < h; ++y) {
        const double v = uvV(y, h);
        for (int x = 0; x < w; ++x) {
          out->f[static_cast<size_t>(y) * w + x] =
              static_cast<float>(radialFactorRaw(m.geometry, uvU(x, w), v));
        }
      }
      return true;
    case MaskType::Brush: {
      // 256² 场每蒙版只建 1 次，再逐像素双线性采样（§1.4 口径）
      std::vector<float> field(static_cast<size_t>(kBrushFieldCell) * kBrushFieldCell);
      buildBrushField256(m.strokes, field.data());
      for (int y = 0; y < h; ++y) {
        const double v = uvV(y, h);
        for (int x = 0; x < w; ++x) {
          out->f[static_cast<size_t>(y) * w + x] = static_cast<float>(
              sampleBrushField256(field.data(), uvU(x, w), v));
        }
      }
      return true;
    }
    case MaskType::LuminanceRange:
      // 范围蒙版基于进入蒙版阶段的像素（S4 末，display 域 u=code/65535）
      for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
          const size_t i0 = (static_cast<size_t>(y) * w + x) * 3;
          const double r = imaging::codeAt(buf, i0) / 65535.0;
          const double g = imaging::codeAt(buf, i0 + 1) / 65535.0;
          const double b = imaging::codeAt(buf, i0 + 2) / 65535.0;
          const double L = 0.2126 * r + 0.7152 * g + 0.0722 * b;
          out->f[static_cast<size_t>(y) * w + x] =
              static_cast<float>(lumRangeFactorRaw(m.range, L));
        }
      }
      return true;
    case MaskType::ColorRange:
      for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
          const size_t i0 = (static_cast<size_t>(y) * w + x) * 3;
          const double r = imaging::codeAt(buf, i0) / 65535.0;
          const double g = imaging::codeAt(buf, i0 + 1) / 65535.0;
          const double b = imaging::codeAt(buf, i0 + 2) / 65535.0;
          out->f[static_cast<size_t>(y) * w + x] =
              static_cast<float>(colorRangeFactorRaw(m.range, r, g, b));
        }
      }
      return true;
    default:
      if (err) {
        *err = "mask '" + m.id + "': 类型 '" + maskTypeName(m.type) +
               "' 本版不支持（v1.3）";
      }
      return false;
  }
}

}  // namespace

bool maskFactorAt(const MaskSpec& m, const imaging::ImageBuf& buf, int x, int y,
                  double* out, std::string* err) {
  if (out == nullptr) {
    if (err) *err = "maskFactorAt: null out";
    return false;
  }
  if (x < 0 || y < 0 || x >= buf.w || y >= buf.h) {
    if (err) *err = "maskFactorAt: 坐标越界 (" + std::to_string(x) + "," + std::to_string(y) + ")";
    return false;
  }
  switch (m.type) {
    case MaskType::Linear:
      *out = linearFactorRaw(m.geometry, uvU(x, buf.w), uvV(y, buf.h));
      return true;
    case MaskType::Radial:
      *out = radialFactorRaw(m.geometry, uvU(x, buf.w), uvV(y, buf.h));
      return true;
    case MaskType::Brush: {
      std::vector<float> field(static_cast<size_t>(kBrushFieldCell) * kBrushFieldCell);
      buildBrushField256(m.strokes, field.data());
      *out = sampleBrushField256(field.data(), uvU(x, buf.w), uvV(y, buf.h));
      return true;
    }
    case MaskType::LuminanceRange: {
      const size_t i0 = (static_cast<size_t>(y) * buf.w + x) * 3;
      const double r = imaging::codeAt(buf, i0) / 65535.0;
      const double g = imaging::codeAt(buf, i0 + 1) / 65535.0;
      const double b = imaging::codeAt(buf, i0 + 2) / 65535.0;
      *out = lumRangeFactorRaw(m.range, 0.2126 * r + 0.7152 * g + 0.0722 * b);
      return true;
    }
    case MaskType::ColorRange: {
      const size_t i0 = (static_cast<size_t>(y) * buf.w + x) * 3;
      *out = colorRangeFactorRaw(m.range, imaging::codeAt(buf, i0) / 65535.0,
                                 imaging::codeAt(buf, i0 + 1) / 65535.0,
                                 imaging::codeAt(buf, i0 + 2) / 65535.0);
      return true;
    }
    default:
      if (err) {
        *err = "mask '" + m.id + "': 类型 '" + maskTypeName(m.type) +
               "' 本版不支持（v1.3）";
      }
      return false;
  }
}

bool buildMaskStack(const json::JsonValue& masks, const imaging::ImageBuf& buf,
                    MaskStack* out, std::string* err) {
  if (out == nullptr) {
    if (err) *err = "buildMaskStack: null out";
    return false;
  }
  out->planes.clear();
  out->appliesLocal.clear();
  if (!masks.isArray()) {
    if (err) *err = "buildMaskStack: masks 必须为数组";
    return false;
  }
  if (buf.w <= 0 || buf.h <= 0 || !buf.valid()) {
    if (err) *err = "buildMaskStack: invalid buffer";
    return false;
  }
  MaskPlane prev;         // 运行中的 fold 平面（f_{i-1}）
  bool havePrev = false;  // 首条有效蒙版前 fold 未定义（首条按 replace，§3.1.3）
  for (size_t i = 0; i < masks.size(); ++i) {
    MaskSpec spec;
    if (!parseMaskSpec(masks.at(i), &spec, err)) return false;
    if (!spec.enabled) {
      // disabled 项不参与 fold：生效权重 = 前一 fold 原样透传，不施加 local
      MaskPlane pass;
      if (havePrev) {
        pass = prev;
      } else {
        pass.w = buf.w;
        pass.h = buf.h;
        pass.f.assign(static_cast<size_t>(buf.w) * buf.h, 0.0f);
      }
      out->planes.push_back(std::move(pass));
      out->appliesLocal.push_back(false);
      continue;
    }
    MaskPlane raw;
    if (!evalRawPlane(spec, buf, &raw, err)) return false;  // A5：不支持类型在此显式失败
    MaskPlane folded;
    folded.w = buf.w;
    folded.h = buf.h;
    folded.f.resize(raw.f.size());
    for (size_t p = 0; p < folded.f.size(); ++p) {
      const double r = raw.f[p];
      double f = r;
      if (havePrev) {
        switch (spec.combine) {
          case MaskCombine::Replace:
            f = r;
            break;
          case MaskCombine::Intersect:
            f = prev.f[p] * r;
            break;
          case MaskCombine::Subtract:
            f = prev.f[p] * (1.0 - r);
            break;
        }
      }
      // else：首条有效蒙版按 replace（combine 对首条无意义，§3.1.3）
      folded.f[p] = static_cast<float>(f);
    }
    out->planes.push_back(folded);
    // subtract 只减面积（传递 fold），不施加 local（§3.1.3 施加循环 continue）
    out->appliesLocal.push_back(spec.combine != MaskCombine::Subtract);
    prev = folded;
    havePrev = true;
  }
  return true;
}

}  // namespace plan
}  // namespace lumina
