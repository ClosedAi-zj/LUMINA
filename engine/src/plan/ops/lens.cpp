// T1.3 lens 节点算子（2 叶，S1 几何域，CPU oracle）
// manualDistortion：径向畸变重映射（线性域解码 → 重采样 → 编码，一次采样）；
// lensProfileEnable：选择器——引擎内置无 profile 数据时 identity（不猜测，T2.x 接 profile 库）。
#include "plan/ops/ops.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace lumina {
namespace plan {
namespace {

class ManualDistortionOp : public ParamOp {
 public:
  ManualDistortionOp() : ParamOp("lens.manualDistortion", Stage::S1Geometry) {}
  bool apply(imaging::ImageBuf* buf, std::string* err) const override {
    if (!checkBuf(buf, err)) return false;
    const double k = num("optics.manualDistortion", 0.0) / 100.0 * 0.3;
    if (k == 0.0) return true;
    // 线性域重映射：dst(归一化 r) ← src(r * (1 + k*r^2))（k>0 桶形）
    const int w = buf->w;
    const int h = buf->h;
    std::vector<float> lin(buf->channelCount());
    for (size_t i = 0; i < lin.size(); ++i) {
      lin[i] = imaging::log16DecodeF(imaging::codeAt(*buf, i), buf->logStep);
    }
    std::vector<float> out(buf->channelCount(), 0.0f);
    const double cx = (w - 1) / 2.0;
    const double cy = (h - 1) / 2.0;
    const double maxR = std::sqrt(cx * cx + cy * cy);
    for (int y = 0; y < h; ++y) {
      for (int x = 0; x < w; ++x) {
        const double dx = x - cx;
        const double dy = y - cy;
        const double r = std::sqrt(dx * dx + dy * dy) / maxR;
        const double rf = r * (1.0 + k * r * r);
        const double sx = cx + dx * (rf / std::max(1e-9, r));
        const double sy = cy + dy * (rf / std::max(1e-9, r));
        // 双线性采样（clamp 出界）
        const int x0 = std::clamp(static_cast<int>(std::floor(sx)), 0, w - 1);
        const int y0 = std::clamp(static_cast<int>(std::floor(sy)), 0, h - 1);
        const int x1 = std::min(x0 + 1, w - 1);
        const int y1 = std::min(y0 + 1, h - 1);
        const double fx = std::clamp(sx - x0, 0.0, 1.0);
        const double fy = std::clamp(sy - y0, 0.0, 1.0);
        float* q = &out[(static_cast<size_t>(y) * w + x) * 3];
        for (int c = 0; c < 3; ++c) {
          const float* p00 = &lin[(static_cast<size_t>(y0) * w + x0) * 3 + c];
          const float* p10 = &lin[(static_cast<size_t>(y0) * w + x1) * 3 + c];
          const float* p01 = &lin[(static_cast<size_t>(y1) * w + x0) * 3 + c];
          const float* p11 = &lin[(static_cast<size_t>(y1) * w + x1) * 3 + c];
          const double top = p00[0] + (p10[0] - p00[0]) * fx;
          const double bot = p01[0] + (p11[0] - p01[0]) * fx;
          q[c] = static_cast<float>(top + (bot - top) * fy);
        }
      }
    }
    // 写回：log16Encode 自带 round-to-栅格（编码点量化，TS oracle 同式）；
    // float 链下 setCodeAt 直写整数码值 float，uint16 路径语义不变。
    for (size_t i = 0; i < out.size(); ++i) {
      imaging::setCodeAt(*buf, i,
                         static_cast<double>(imaging::log16Encode(out[i], buf->logStep)));
    }
    return true;
  }
};

class LensProfileOp : public ParamOp {
 public:
  LensProfileOp() : ParamOp("lens.profile", Stage::S1Geometry) {}
  bool apply(imaging::ImageBuf* buf, std::string* err) const override {
    if (!checkBuf(buf, err)) return false;
    // enable=1 但无 profile 数据：identity 不猜测（零静默：不报错不丢真）。
    // T2.x 接入 profile 库后在此实装。
    (void)num("optics.lensProfileEnable", 0.0);
    return true;
  }
};

}  // namespace

bool registerLensOps(OpTable* table, std::string* err) {
  bool ok = true;
  ok = table->registerOp(std::make_shared<LensProfileOp>(), err) && ok;
  ok = table->registerOp(std::make_shared<ManualDistortionOp>(), err) && ok;
  return ok;
}

}  // namespace plan
}  // namespace lumina
