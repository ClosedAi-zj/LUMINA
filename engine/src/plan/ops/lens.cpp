// T1.3 lens 节点算子（2 叶，S1 几何域，CPU oracle）
// T2.10 收口：lens.manualDistortion 改为**废弃转发** GeometryChain（单参数畸变），
// 生产 Plan 只产出 geometry.apply（ops/geometry.cpp GeometryApplyOp）。
// T2.11：LensProfileOp 实装为参数入口 + 未命中显式告知——profile 求值本身在
// GeometryChain（S1 单次采样，O4；架构 §4.3 / M2-架构设计.md:924），本算子
// 负责：enable≠0 时尝试匹配 LensProfileDb，未命中 → identity + warning
// （零静默：lensProfileLastWarning 可观测，UI 据此置灰，M30）。
#include "plan/ops/ops.h"

#include "plan/geometry_chain.h"
#include "plan/lens_profile.h"

#include <algorithm>

namespace lumina {
namespace plan {
namespace {

class ManualDistortionOp : public ParamOp {
 public:
  ManualDistortionOp() : ParamOp("lens.manualDistortion", Stage::S1Geometry) {}
  bool apply(imaging::ImageBuf* buf, std::string* err) const override {
    if (!checkBuf(buf, err)) return false;
    // 废弃转发：单参数畸变走几何链（k = amt/100*0.3，与 oracle/GPU 同式）。
    // 与 T1.3 旧实现的两步重映射相比，合并采样消除了中间 log16 量化（T2.10 偏差④收口）。
    GeometryChainParams p;
    p.manualDistortion = num("optics.manualDistortion", 0.0);
    return applyGeometryChain(buf, p, nullptr, err);
  }
};

class LensProfileOp : public ParamOp {
 public:
  LensProfileOp() : ParamOp("lens.profile", Stage::S1Geometry) {}
  bool apply(imaging::ImageBuf* buf, std::string* err) const override {
    if (!checkBuf(buf, err)) return false;
    const double enable = num("optics.lensProfileEnable", 0.0);
    if (enable == 0.0) return true;  // 选择器关：恒等
    // 匹配尝试（像素级校正由 geometry.apply 的 GeometryChain 完成——单次采样）。
    LensProfile matched;
    if (!LensProfileDb::global().match(params().get("optics.lensProfileMake").asString(),
                                       params().get("optics.lensProfileModel").asString(),
                                       params().get("optics.lensProfileLensMake").asString(),
                                       params().get("optics.lensProfileLensModel").asString(),
                                       num("optics.lensProfileFocalMm", 0.0), &matched)) {
      // 未命中：identity + 显式 warning（零静默，不猜测）
      lensProfileSetWarning("未检测到镜头配置（lens.profile 未命中）：make=" +
                            params().get("optics.lensProfileMake").asString() +
                            " model=" + params().get("optics.lensProfileModel").asString());
    }
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
