// T1.3 mask 容器节点（1 叶）+ T2.6 蒙版数学消费端（MaskApplyOp 实装）。
// §3.3 伪码：layer = Op[node](base)；next = mix(base, Blend(layer, base, blendMode),
//                                              a * opacity/100)。
// T2.6（架构 §3.1.3/§3.1.4）：MaskApplyOp = 蒙版局部调整的引擎权威 CPU 实现——
// buildMaskStack 产 fold 平面 → 按数组序逐蒙版施加 9 局部叶（display 域闭式逐像素，
// clarity 用共享盒式模糊平面，每次渲染只算 1 次）。GPU mask pass（T2.7）逐字镜像。
#include "plan/ops/ops.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include "plan/ops/mask_field.h"

namespace lumina {
namespace plan {

// 蒙版求值入口（CPU/GPU 共用同一语义）：
// - op：宿主节点算子（已 setParams）
// - base：主链当前缓冲；mask：0..1 蒙版平面（同 w*h；null = 全 1）
// - opacity 0..100；blendMode：normal|multiply|screen|overlay|softLight
// 返回混合后缓冲写入 out（可与 base 同对象）。
bool applyMasked(const Operator& op, const imaging::ImageBuf& base,
                 const std::vector<float>* mask, int opacity,
                 const std::string& blendMode, imaging::ImageBuf* out,
                 std::string* err) {
  imaging::ImageBuf layer = base;
  if (!op.apply(&layer, err)) return false;
  if (out->w != layer.w || out->h != layer.h ||
      out->channelCount() != layer.channelCount() ||
      out->floatMode() != layer.floatMode()) {
    out->w = layer.w;
    out->h = layer.h;
    out->space = layer.space;
    out->logStep = layer.logStep;
    out->data.clear();
    out->dataF.clear();
    if (layer.floatMode()) {
      out->dataF.assign(layer.dataF.size(), 0.0f);
    } else {
      out->data.assign(layer.data.size(), 0);
    }
  }
  const double a = std::max(0.0, std::min(1.0, opacity / 100.0));
  const bool normalOnly = (mask == nullptr);
  const size_t n = layer.channelCount();
  for (size_t i = 0; i < n; ++i) {
    const double lv = imaging::codeAt(layer, i);
    const double bv = imaging::codeAt(base, i);
    double blended = lv;
    if (blendMode == "multiply") {
      blended = bv * lv / 65535.0;
    } else if (blendMode == "screen") {
      blended = bv + lv - bv * lv / 65535.0;
    } else if (blendMode == "overlay" || blendMode == "softLight") {
      // 感知码域近似：0.5 为界
      const double t = bv / 65535.0;
      const double s = lv / 65535.0;
      blended = (t < 0.5 ? 2.0 * t * s : 1.0 - 2.0 * (1.0 - t) * (1.0 - s)) * 65535.0;
    }
    double alpha = a;
    if (!normalOnly) {
      alpha *= (*mask)[i / 3];  // 蒙版平面 per-pixel（交错 RGB 共用）
    }
    imaging::setCodeAt(*out, i, bv + (blended - bv) * alpha);
  }
  return true;
}

namespace {

// ---- 9 局部叶参数（§3.1.4 公式表；键口径 = sidecar masks[*].local）----
struct LocalParams {
  double exposure = 0.0;
  double contrast = 0.0;
  double highlights = 0.0;
  double shadows = 0.0;
  double temperature = 0.0;
  double tint = 0.0;
  double saturation = 0.0;
  double clarity = 0.0;
  double dehaze = 0.0;
  bool allZero() const {
    return exposure == 0.0 && contrast == 0.0 && highlights == 0.0 &&
           shadows == 0.0 && temperature == 0.0 && tint == 0.0 &&
           saturation == 0.0 && clarity == 0.0 && dehaze == 0.0;
  }
};

LocalParams readLocal(const json::JsonValue& local) {
  LocalParams p;
  p.exposure = local.get("exposure").asDouble(0.0);
  p.contrast = local.get("contrast").asDouble(0.0);
  p.highlights = local.get("highlights").asDouble(0.0);
  p.shadows = local.get("shadows").asDouble(0.0);
  p.temperature = local.get("temperature").asDouble(0.0);
  p.tint = local.get("tint").asDouble(0.0);
  p.saturation = local.get("saturation").asDouble(0.0);
  p.clarity = local.get("clarity").asDouble(0.0);
  p.dehaze = local.get("dehaze").asDouble(0.0);
  return p;
}

// 9 叶按 §3.1.4 顺序施加到单像素（u = code/65535，display 域；w = 生效权重 ∈[-1,1]）。
// blur：共享盒式模糊平面（3 通道拼接，码域值），仅 clarity 消费；null = 本渲染无 clarity。
// 色相窗口内亮度 L 在 highlights/shadows 施加时按当时像素状态现算（GPU 镜像同式）。
void applyLocalPixel(double u[3], double w, const LocalParams& p, const float* blur,
                     int x, int y, int W, int H) {
  // 1 exposure：u *= 2^(E*w)（单位 EV，与全局 exposure 同语义）
  if (p.exposure != 0.0) {
    const double g = std::pow(2.0, p.exposure * w);
    u[0] *= g;
    u[1] *= g;
    u[2] *= g;
  }
  // 2 contrast：以 0.5 为轴
  if (p.contrast != 0.0) {
    const double k = 1.0 + p.contrast / 100.0 * 0.5 * w;
    for (int c = 0; c < 3; ++c) u[c] = (u[c] - 0.5) * k + 0.5;
  }
  // 3 highlights：u += H/100*0.4*w*hw*(1-u)，hw = smooth01(L,0.45,1)
  if (p.highlights != 0.0) {
    const double L = 0.2126 * u[0] + 0.7152 * u[1] + 0.0722 * u[2];
    const double hw = smooth01(L, 0.45, 1.0);
    const double k = p.highlights / 100.0 * 0.4 * w * hw;
    for (int c = 0; c < 3; ++c) u[c] += k * (1.0 - u[c]);
  }
  // 4 shadows：u += Sh/100*0.4*w*sw*(1-u)，sw = 1 - smooth01(L,0,0.55)
  if (p.shadows != 0.0) {
    const double L = 0.2126 * u[0] + 0.7152 * u[1] + 0.0722 * u[2];
    const double sw = 1.0 - smooth01(L, 0.0, 0.55);
    const double k = p.shadows / 100.0 * 0.4 * w * sw;
    for (int c = 0; c < 3; ++c) u[c] += k * (1.0 - u[c]);
  }
  // 5 temperature：u_r *= 2^(T/100*0.10*w)；u_b *= 2^(-T/100*0.10*w)（display 域近似）
  if (p.temperature != 0.0) {
    const double g = p.temperature / 100.0 * 0.10 * w;
    u[0] *= std::pow(2.0, g);
    u[2] *= std::pow(2.0, -g);
  }
  // 6 tint：u_g *= 2^(Ti/100*0.05*w)
  if (p.tint != 0.0) {
    u[1] *= std::pow(2.0, p.tint / 100.0 * 0.05 * w);
  }
  // 7 saturation：HSL 往返，s *= 1 + Sat/100*0.6*w（与 color.hsl 同一 HSL 实现）
  if (p.saturation != 0.0) {
    // HSL 域要求 [0,1]；前面叶可能越界，先截断（GPU 镜像同口径）
    for (int c = 0; c < 3; ++c) u[c] = clamp01(u[c]);
    double h = 0.0, s = 0.0, l = 0.0;
    rgbToHsl(u[0], u[1], u[2], &h, &s, &l);
    s *= 1.0 + p.saturation / 100.0 * 0.6 * w;
    if (s < 0.0) s = 0.0;
    if (s > 1.0) s = 1.0;
    double r = 0.0, g = 0.0, b = 0.0;
    hslToRgb(h, s, l, &r, &g, &b);
    u[0] = r;
    u[1] = g;
    u[2] = b;
  }
  // 8 clarity：u += Cl/100*0.6*w*(u - B)；B = 共享盒式模糊平面（半径 10，码域）
  if (p.clarity != 0.0 && blur != nullptr) {
    const double k = p.clarity / 100.0 * 0.6 * w;
    const size_t plane = static_cast<size_t>(W) * H;
    for (int c = 0; c < 3; ++c) {
      const double B = blur[static_cast<size_t>(c) * plane +
                            static_cast<size_t>(y) * W + x] / 65535.0;
      u[c] += k * (u[c] - B);
    }
  }
  // 9 dehaze：d≥0 黑场下压；d<0 加雾（与全局 dehaze 同向）
  if (p.dehaze != 0.0) {
    const double d = p.dehaze / 100.0 * w;
    for (int c = 0; c < 3; ++c) {
      u[c] = d >= 0.0 ? (u[c] - 0.15 * d) / (1.0 - 0.15 * d)
                      : u[c] * (1.0 + 0.3 * d) + 0.10 * (-d);
    }
  }
}

// mask.apply：蒙版局部调整引擎权威（T2.6 实装；此前的 identity 占位退役）。
// 参数：{"masks": [ sidecar masks[*] 原样 ]}（compilePlan 的 opForParam 路由）。
// 无 masks 键 / 空数组 = identity（无蒙版编辑的照片不进蒙版数学）。
// 任何不支持类型（depth/ai*/未知）显式失败（A5 零静默），禁止回落画笔分支。
class MaskApplyOp : public ParamOp {
 public:
  MaskApplyOp() : ParamOp("mask.apply", Stage::S4Color) {}
  bool apply(imaging::ImageBuf* buf, std::string* err) const override {
    if (!checkBuf(buf, err)) return false;
    const json::JsonValue& masks = params().get("masks");
    if (!masks.isArray() || masks.empty()) return true;  // 无蒙版 = identity
    MaskStack stack;
    if (!buildMaskStack(masks, *buf, &stack, err)) return false;
    if (stack.planes.empty()) return true;

    const int W = buf->w;
    const int H = buf->h;
    const size_t planeSz = static_cast<size_t>(W) * H;

    // 解析 local（buildMaskStack 已验证过全部项，这里失败即内部错误，仍显式报错）
    std::vector<LocalParams> locals(masks.size());
    std::vector<const MaskPlane*> planes(masks.size(), nullptr);
    bool needBlur = false;
    for (size_t i = 0; i < masks.size(); ++i) {
      MaskSpec spec;
      if (!parseMaskSpec(masks.at(i), &spec, err)) return false;
      locals[i] = readLocal(spec.local);
      planes[i] = &stack.planes[i];
      if (stack.appliesLocal[i] && !locals[i].allZero() &&
          std::fabs(locals[i].clarity) > 1e-12) {
        needBlur = true;
      }
    }

    // 共享盒式模糊平面 B（§1.4：半径 10 与 basic.clarity 同半径；每次渲染只算 1 次，
    // N 个蒙版共用）。进入蒙版阶段的码域快照，3 通道拼接存储。
    std::vector<float> blur;
    if (needBlur) {
      constexpr int kClarityRadius = 10;
      blur.assign(planeSz * 3, 0.0f);
      std::vector<float> src(planeSz);
      std::vector<float> dst(planeSz);
      for (int c = 0; c < 3; ++c) {
        for (size_t p = 0; p < planeSz; ++p) {
          src[p] = imaging::codeAt(*buf, p * 3 + c);
        }
        boxBlurPlane(src.data(), dst.data(), W, H, kClarityRadius);
        std::copy(dst.begin(), dst.end(), blur.begin() + static_cast<size_t>(c) * planeSz);
      }
    }

    // 施加循环（§3.1.3）：按数组序逐蒙版；w = clamp(f_i * opacity/100 * amount/100, -1, 1)；
    // subtract（appliesLocal=false）只参与 fold，不施加 local。
    for (size_t i = 0; i < masks.size(); ++i) {
      if (!stack.appliesLocal[i]) continue;
      const LocalParams& lp = locals[i];
      if (lp.allZero()) continue;  // 全零 local = 该蒙版无像素效果
      const MaskPlane& pl = *planes[i];
      const double opa = specOpacity(masks.at(i));
      const double amt = specAmount(masks.at(i));
      for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
          const double f = pl.f[static_cast<size_t>(y) * W + x];
          double w = f * opa * amt;
          if (w < -1.0) w = -1.0;
          if (w > 1.0) w = 1.0;
          if (w == 0.0) continue;
          const size_t i0 = (static_cast<size_t>(y) * W + x) * 3;
          double u[3] = {imaging::codeAt(*buf, i0) / 65535.0,
                         imaging::codeAt(*buf, i0 + 1) / 65535.0,
                         imaging::codeAt(*buf, i0 + 2) / 65535.0};
          applyLocalPixel(u, w, lp, blur.empty() ? nullptr : blur.data(), x, y, W, H);
          for (int c = 0; c < 3; ++c) {
            imaging::setCodeAt(*buf, i0 + c, u[c] * 65535.0);
          }
        }
      }
    }
    return true;
  }

 private:
  // opacity/amount 截断口径与 parseMaskSpec 一致（此处只做施加端的二次取值）
  static double specOpacity(const json::JsonValue& j) {
    double v = j.get("opacity").asDouble(100.0);
    if (v < 0.0) v = 0.0;
    if (v > 100.0) v = 100.0;
    return v / 100.0;
  }
  static double specAmount(const json::JsonValue& j) {
    double v = j.get("amount").asDouble(100.0);
    if (v < -100.0) v = -100.0;
    if (v > 100.0) v = 100.0;
    return v / 100.0;
  }
};

}  // namespace

bool registerMaskOps(OpTable* table, std::string* err) {
  return table->registerOp(std::make_shared<MaskApplyOp>(), err);
}

}  // namespace plan
}  // namespace lumina
