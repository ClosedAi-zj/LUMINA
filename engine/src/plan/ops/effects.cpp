// T1.3 effects 节点算子（6 叶：裁后暗角 3 + 颗粒 3，S6，CPU oracle 感知码域）
// 暗角：径向衰减（midpoint 起点 / feather 展宽），amount<0 压暗边缘；
// 颗粒：确定性哈希噪声（跨运行稳定），size 控制块度、frequency 调制幅度。
#include "plan/ops/ops.h"

#include <algorithm>
#include <cmath>

namespace lumina {
namespace plan {
namespace {

class VignetteOp : public ParamOp {
 public:
  VignetteOp() : ParamOp("effects.vignette", Stage::S6Effects) {}
  bool apply(imaging::ImageBuf* buf, std::string* err) const override {
    if (!checkBuf(buf, err)) return false;
    const double amount = params_.get("optics.vignetteAmount").asDouble(0.0);
    if (amount == 0.0) return true;
    const double midpoint = params_.get("optics.vignetteMidpoint").asDouble(50.0) / 100.0;
    const double feather = std::max(1.0, params_.get("optics.vignetteFeather").asDouble(50.0));
    const double cx = (buf->w - 1) / 2.0;
    const double cy = (buf->h - 1) / 2.0;
    const double maxR = std::sqrt(cx * cx + cy * cy);
    for (int y = 0; y < buf->h; ++y) {
      for (int x = 0; x < buf->w; ++x) {
        const double r = std::sqrt((x - cx) * (x - cx) + (y - cy) * (y - cy)) / maxR;
        // midpoint 起、feather 控制衰减带宽度
        const double t = smooth01(r, midpoint, midpoint + feather / 100.0);
        const double f = 1.0 + amount / 100.0 * t;
        for (int c = 0; c < 3; ++c) {
          const size_t idx = (static_cast<size_t>(y) * buf->w + x) * 3 + c;
          // 感知码域缩放（amount 负 → 边缘变暗）
          imaging::setCodeAt(*buf, idx,
                             static_cast<double>(imaging::codeAt(*buf, idx)) *
                                 std::clamp(f, 0.0, 2.0));
        }
      }
    }
    return true;
  }
};

class GrainOp : public ParamOp {
 public:
  GrainOp() : ParamOp("effects.grain", Stage::S6Effects) {}
  bool apply(imaging::ImageBuf* buf, std::string* err) const override {
    if (!checkBuf(buf, err)) return false;
    const double amount = params_.get("optics.grainAmount").asDouble(0.0);
    if (amount == 0.0) return true;
    const double size = std::max(1.0, params_.get("optics.grainSize").asDouble(0.0));
    const double freq = params_.get("optics.grainFrequency").asDouble(0.0);
    const double amp = amount / 100.0 * 1500.0 * (1.0 + freq / 100.0 * 0.5);
    for (int y = 0; y < buf->h; ++y) {
      for (int x = 0; x < buf->w; ++x) {
        const int bx = static_cast<int>(x / size);
        const int by = static_cast<int>(y / size);
        const double n = (hashNoise(bx, by, 7) * 2.0 - 1.0);  // [-1,1) 零均值
        const double d = n * amp;
        for (int c = 0; c < 3; ++c) {
          const size_t idx = (static_cast<size_t>(y) * buf->w + x) * 3 + c;
          // 亮度颗粒：RGB 同偏移
          imaging::setCodeAt(*buf, idx, static_cast<double>(imaging::codeAt(*buf, idx)) + d);
        }
      }
    }
    return true;
  }
};

}  // namespace

bool registerEffectsOps(OpTable* table, std::string* err) {
  bool ok = true;
  ok = table->registerOp(std::make_shared<VignetteOp>(), err) && ok;
  ok = table->registerOp(std::make_shared<GrainOp>(), err) && ok;
  return ok;
}

}  // namespace plan
}  // namespace lumina
