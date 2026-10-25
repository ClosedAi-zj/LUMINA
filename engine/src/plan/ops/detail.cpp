// T1.3 detail 节点算子（7 叶：NR 3 叶 S1.5 + 锐化 4 叶 S5，CPU oracle）
// NR：色度模糊（colorNR/colorNRDetail）+ 亮度平滑（luminanceSmoothing），线性光码域。
// 锐化：反遮罩锐化（unsharp mask），半径/数量/细节/蒙版阈值。全部 identity@0。
#include "plan/ops/ops.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace lumina {
namespace plan {
namespace {

// 平面提取/回写辅助
void extractPlane(const imaging::ImageBuf& buf, int c, std::vector<float>* out) {
  out->resize(static_cast<size_t>(buf.w) * buf.h);
  for (int y = 0; y < buf.h; ++y) {
    for (int x = 0; x < buf.w; ++x) {
      (*out)[static_cast<size_t>(y) * buf.w + x] =
          static_cast<float>(buf.data[(static_cast<size_t>(y) * buf.w + x) * 3 + c]);
    }
  }
}
void writePlaneDelta(imaging::ImageBuf* buf, int c, const std::vector<float>& delta) {
  for (int y = 0; y < buf->h; ++y) {
    for (int x = 0; x < buf->w; ++x) {
      const size_t idx = (static_cast<size_t>(y) * buf->w + x) * 3 + c;
      const double v = buf->data[idx] + delta[static_cast<size_t>(y) * buf->w + x];
      buf->data[idx] = static_cast<uint16_t>(std::clamp(v, 0.0, 65535.0) + 0.5);
    }
  }
}

// NR（S1.5）：色度模糊 + 亮度平滑
class DenoiseOp : public ParamOp {
 public:
  DenoiseOp() : ParamOp("detail.denoise", Stage::S15Denoise) {}
  bool apply(imaging::ImageBuf* buf, std::string* err) const override {
    if (!checkBuf(buf, err)) return false;
    const double cnr = num("detail.colorNR", 0.0);
    const double cnrDetail = num("detail.colorNRDetail", 50.0);
    const double luma = num("detail.luminanceSmoothing", 0.0);
    if (cnr == 0.0 && luma == 0.0) return true;
    const int w = buf->w;
    const int h = buf->h;
    // 色度 = 通道 - 亮度；模糊色度不动亮度（去色噪）
    std::vector<float> lum(static_cast<size_t>(w) * h);
    for (int y = 0; y < h; ++y) {
      for (int x = 0; x < w; ++x) {
        const uint16_t* p = &buf->data[(static_cast<size_t>(y) * w + x) * 3];
        lum[static_cast<size_t>(y) * w + x] =
            static_cast<float>((p[0] + p[1] + p[2]) / 3.0);
      }
    }
    const int chromaR = std::max(1, static_cast<int>(cnr / 100.0 * 5.0 + 0.5));
    const int lumaR = std::max(1, static_cast<int>(luma / 100.0 * 3.0 + 0.5));
    // detail 高 → 保留更多边缘（模糊结果与原值混合比例降低）
    const double chromaKeep = cnrDetail / 100.0 * 0.5;  // 0..0.5
    std::vector<float> plane, blurred, delta;
    plane.resize(lum.size());
    blurred.resize(lum.size());
    delta.resize(lum.size());
    for (int c = 0; c < 3; ++c) {
      if (cnr > 0.0) {
        extractPlane(*buf, c, &plane);
        for (size_t i = 0; i < plane.size(); ++i) plane[i] -= lum[i];  // 色度
        boxBlurPlane(plane.data(), blurred.data(), w, h, chromaR);
        for (size_t i = 0; i < delta.size(); ++i) {
          delta[i] = blurred[i] * (1.0 - chromaKeep) - plane[i] * (1.0 - chromaKeep);
        }
        // 上面的 delta 直接把色度替换为模糊值（保留 chromaKeep 比例原值）
        writePlaneDelta(buf, c, delta);
      }
      if (luma > 0.0) {
        extractPlane(*buf, c, &plane);
        boxBlurPlane(plane.data(), blurred.data(), w, h, lumaR);
        for (size_t i = 0; i < delta.size(); ++i) {
          delta[i] = (blurred[i] - plane[i]) * (luma / 100.0);
        }
        writePlaneDelta(buf, c, delta);
      }
    }
    return true;
  }
};

// 锐化（S5）：unsharp mask + 蒙版阈值
class SharpenOp : public ParamOp {
 public:
  SharpenOp() : ParamOp("detail.sharpen", Stage::S5Detail) {}
  bool apply(imaging::ImageBuf* buf, std::string* err) const override {
    if (!checkBuf(buf, err)) return false;
    const double amount = num("detail.sharpness", 0.0);
    if (amount == 0.0) return true;
    const double radius = std::max(1.0, num("detail.sharpenRadius", 1.0));
    const double detail = num("detail.sharpenDetail", 25.0);
    const double masking = num("detail.sharpenMasking", 0.0);
    const int w = buf->w;
    const int h = buf->h;
    const int r = std::max(1, static_cast<int>(radius + 0.5));
    std::vector<float> plane, blurred, delta;
    plane.resize(static_cast<size_t>(w) * h);
    blurred.resize(plane.size());
    delta.resize(plane.size());
    const double k = amount / 100.0 * 0.9;               // 数量
    const double dBoost = 1.0 + detail / 100.0 * 0.5;    // 细节增强残差
    const double threshold = masking / 100.0 * 30.0;     // 蒙版阈值（码值差）
    for (int c = 0; c < 3; ++c) {
      extractPlane(*buf, c, &plane);
      boxBlurPlane(plane.data(), blurred.data(), w, h, r);
      for (size_t i = 0; i < plane.size(); ++i) {
        const double resid = (plane[i] - blurred[i]) * dBoost;
        delta[i] = std::fabs(resid) < threshold ? 0.0 : resid * k;
      }
      writePlaneDelta(buf, c, delta);
    }
    return true;
  }
};

}  // namespace

bool registerDetailOps(OpTable* table, std::string* err) {
  bool ok = true;
  ok = table->registerOp(std::make_shared<DenoiseOp>(), err) && ok;
  ok = table->registerOp(std::make_shared<SharpenOp>(), err) && ok;
  return ok;
}

}  // namespace plan
}  // namespace lumina
