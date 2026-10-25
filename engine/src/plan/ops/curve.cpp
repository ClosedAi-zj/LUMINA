// T1.3 curve 节点算子（11 叶，CPU oracle，S4 感知码域）
// 曲线 LUT 作用在 log16 码域（code/65535 → 0..255 输入）——对 S3 前后均语义成立。
// 点曲线复用 9e5a9db parseCurve 产物（{x,y}[]）；参数曲线按四分区权重（LR 语义）。
#include "plan/ops/ops.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace lumina {
namespace plan {
namespace {

// {x,y}[] → 256-entry LUT（0..255 域）；<2 点或 null → 恒等 LUT
std::vector<double> lutFromPoints(const json::JsonValue& pts) {
  std::vector<double> lut(256);
  for (int i = 0; i < 256; ++i) lut[i] = i;
  if (!pts.isArray() || pts.size() < 2) return lut;
  std::vector<std::pair<double, double>> p;
  for (const auto& pt : pts.arrayItems()) {
    p.push_back({pt.get("x").asDouble(), pt.get("y").asDouble()});
  }
  std::sort(p.begin(), p.end());
  if (p.front().first > 0) p.insert(p.begin(), {0.0, p.front().second});
  if (p.back().first < 255) p.push_back({255.0, p.back().second});
  for (int i = 0; i < 256; ++i) {
    const double u = static_cast<double>(i);
    size_t k = 0;
    while (k + 1 < p.size() && p[k + 1].first < u) ++k;
    const double t = (u - p[k].first) / std::max(1e-9, p[k + 1].first - p[k].first);
    lut[i] = p[k].second + (p[k + 1].second - p[k].second) * t;
  }
  return lut;
}

// 点曲线基座：curveKey = "curve.toneCurveRGB" 等；channel: -1=全部
class PointCurveOp : public ParamOp {
 public:
  PointCurveOp(const char* n, int channel)
      : ParamOp(n, Stage::S4Color), ch_(channel) {}
  bool apply(imaging::ImageBuf* buf, std::string* err) const override {
    if (!checkBuf(buf, err)) return false;
    const std::vector<double> lut = lutFromPoints(params_.get(n_));
    bool identity = true;
    for (int i = 0; i < 256; ++i) {
      if (std::fabs(lut[i] - i) > 1e-9) identity = false;
    }
    if (identity) return true;
    for (size_t i = 0; i < buf->data.size(); ++i) {
      const bool applies = ch_ < 0 || (i % 3) == static_cast<size_t>(ch_);
      if (!applies) continue;
      const double u = buf->data[i] / 65535.0 * 255.0;
      const double v = lut[std::clamp(static_cast<int>(u + 0.5), 0, 255)];
      buf->data[i] = static_cast<uint16_t>(std::clamp(v / 255.0 * 65535.0, 0.0, 65535.0) + 0.5);
    }
    return true;
  }
 private:
  int ch_;  // -1 = RGB 复合，0/1/2 = R/G/B
};

// 参数曲线（单算子消费 7 叶：4 个 amount + 3 个分界，LR 语义四分区权重）。
// 叶路径 = "parametric.<zone>"（与 param-leaves.json 一致）；分界默认 25/50/75。
class ParametricOp : public ParamOp {
 public:
  ParametricOp() : ParamOp("curve.parametric", Stage::S4Color) {}
  bool apply(imaging::ImageBuf* buf, std::string* err) const override {
    if (!checkBuf(buf, err)) return false;
    const double hi = num("parametric.highlights", 0.0);
    const double li = num("parametric.lights", 0.0);
    const double da = num("parametric.darks", 0.0);
    const double sh = num("parametric.shadows", 0.0);
    if (hi == 0.0 && li == 0.0 && da == 0.0 && sh == 0.0) return true;
    const double s0 = num("parametric.shadowSplit", 25.0) / 100.0 * 255.0;
    const double s1 = num("parametric.midSplit", 75.0) / 100.0 * 255.0;
    const double sL = num("parametric.lightSplit", 50.0) / 100.0 * 255.0;
    for (size_t i = 0; i < buf->data.size(); ++i) {
      const double u = buf->data[i] / 65535.0 * 255.0;
      // 四分区权重（平滑互补分区；b0=shadowSplit, b1=lightSplit, b2=midSplit）
      const double t0 = smooth01(u, 0.0, s0);
      const double t1 = smooth01(u, s0, sL);
      const double t2 = smooth01(u, sL, s1);
      const double wHi = t2;
      const double wLi = t1 - t2;
      const double wDa = t0 - t1;
      const double wSh = 1.0 - t0;
      const double delta = (hi * wHi + li * wLi + da * wDa + sh * wSh) / 100.0 * 60.0;
      const double v = std::clamp(u + delta, 0.0, 255.0);
      buf->data[i] = static_cast<uint16_t>(v / 255.0 * 65535.0 + 0.5);
    }
    return true;
  }
};

}  // namespace

bool registerCurveOps(OpTable* table, std::string* err) {
  bool ok = true;
  ok = table->registerOp(std::make_shared<PointCurveOp>("curve.toneCurveRGB", -1), err) && ok;
  ok = table->registerOp(std::make_shared<PointCurveOp>("curve.toneCurveR", 0), err) && ok;
  ok = table->registerOp(std::make_shared<PointCurveOp>("curve.toneCurveG", 1), err) && ok;
  ok = table->registerOp(std::make_shared<PointCurveOp>("curve.toneCurveB", 2), err) && ok;
  ok = table->registerOp(std::make_shared<ParametricOp>(), err) && ok;
  return ok;
}

}  // namespace plan
}  // namespace lumina
