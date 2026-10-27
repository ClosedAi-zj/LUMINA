// T1.3 lens 节点算子（2 叶，S1 几何域，CPU oracle）
// T2.10 收口：lens.manualDistortion 改为**废弃转发** GeometryChain（单参数畸变），
// 生产 Plan 只产出 geometry.apply（ops/geometry.cpp GeometryApplyOp）。
// lensProfileEnable：选择器——引擎内置无 profile 数据时 identity（不猜测，T2.11 接库）。
#include "plan/ops/ops.h"

#include "plan/geometry_chain.h"

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
    // enable=1 但无 profile 数据：identity 不猜测（零静默：不报错不丢真）。
    // T2.11 接入 profile 库后在此实装（子步 1，几何链已预留槽位）。
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
