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

// T2.9 曲线 LUT 栅格：0..255 曲线域上 1024 点重采样（此前 256 点把 16bit 输入
// 量化到 8bit 档位，逐码值扫描呈周期性平台 + 242 LSB 级跳变）。lut[i] = 曲线在
// 输入 x = i*255/(N-1) 处的输出；查表 = 分段线性插值（pos = code/65535*(N-1)），
// 线性插值天然单调无过冲，控制点折线经 1024 栅格重采样不引入新形状。
constexpr int kCurveLutSize = 1024;

// {x,y}[] → kCurveLutSize-entry LUT（0..255 域）；<2 点或 null → 恒等 LUT
std::vector<double> lutFromPoints(const json::JsonValue& pts) {
  std::vector<double> lut(kCurveLutSize);
  for (int i = 0; i < kCurveLutSize; ++i) lut[i] = i * 255.0 / (kCurveLutSize - 1);
  if (!pts.isArray() || pts.size() < 2) return lut;
  std::vector<std::pair<double, double>> p;
  for (const auto& pt : pts.arrayItems()) {
    p.push_back({pt.get("x").asDouble(), pt.get("y").asDouble()});
  }
  std::sort(p.begin(), p.end());
  if (p.front().first > 0) p.insert(p.begin(), {0.0, p.front().second});
  if (p.back().first < 255) p.push_back({255.0, p.back().second});
  for (int i = 0; i < kCurveLutSize; ++i) {
    const double u = i * 255.0 / (kCurveLutSize - 1);
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
    for (int i = 0; i < kCurveLutSize; ++i) {
      if (std::fabs(lut[i] - i * 255.0 / (kCurveLutSize - 1)) > 1e-9) identity = false;
    }
    if (identity) return true;
    for (size_t i = 0; i < buf->channelCount(); ++i) {
      const bool applies = ch_ < 0 || (i % 3) == static_cast<size_t>(ch_);
      if (!applies) continue;
      const double pos = imaging::codeAt(*buf, i) / 65535.0 * (kCurveLutSize - 1);
      const int k = std::clamp(static_cast<int>(pos), 0, kCurveLutSize - 1);
      const int k1 = std::min(k + 1, kCurveLutSize - 1);
      const double f = std::max(0.0, pos - static_cast<double>(k));
      const double v = lut[k] + (lut[k1] - lut[k]) * f;
      imaging::setCodeAt(*buf, i, v / 255.0 * 65535.0);
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
    for (size_t i = 0; i < buf->channelCount(); ++i) {
      const double u = imaging::codeAt(*buf, i) / 65535.0 * 255.0;
      // 四分区权重（平滑互补分区；b0=shadowSplit, b1=lightSplit, b2=midSplit）
      const double t0 = smooth01(u, 0.0, s0);
      const double t1 = smooth01(u, s0, sL);
      const double t2 = smooth01(u, sL, s1);
      const double wHi = t2;
      const double wLi = t1 - t2;
      const double wDa = t0 - t1;
      const double wSh = 1.0 - t0;
      const double delta = (hi * wHi + li * wLi + da * wDa + sh * wSh) / 100.0 * 60.0;
      imaging::setCodeAt(*buf, i, (u + delta) / 255.0 * 65535.0);
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
