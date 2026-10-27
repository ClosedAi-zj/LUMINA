// T1.3/T2.10 geometry 节点算子（13 叶，S1，CPU oracle）
// T2.10 收口：新增单一算子 geometry.apply（架构 §4.3 子序 1畸变→…→4透视→5aspect
// →6裁剪，一次重采样，O3/O4），实现基座在 plan/geometry_chain.cpp。
// 旧算子名 geometry.crop / geometry.perspective 保留注册为**废弃转发**（test_ops93/
// test_op_table 兼容）：内部转发 GeometryChain 的单参数调用，行为 = 旧独立算子
// （单参数时几何链退化为原单应）。生产 Plan（compilePlan → opForParam）只产出
// geometry.apply，S1 路由单一来源。
// 参数键：geometry.hasCrop/cropLeft/cropTop/cropRight/cropBottom/cropAngle 与
// geometry.perspective.{x,y,horizontal,vertical,rotate,scale,aspect} + optics.*。
#include "plan/ops/ops.h"

#include "plan/geometry_chain.h"

#include <algorithm>

namespace lumina {
namespace plan {
namespace {

// T2.10：S1 单一算子（表行 2「镜头 profile 畸变」块的入口；行 2–6 = 本算子内部子步）
class GeometryApplyOp : public ParamOp {
 public:
  GeometryApplyOp() : ParamOp("geometry.apply", Stage::S1Geometry) {}
  bool apply(imaging::ImageBuf* buf, std::string* err) const override {
    if (!checkBuf(buf, err)) return false;
    const GeometryChainParams p = collectGeometryChainParams(params_);
    return applyGeometryChain(buf, p, nullptr, err);
  }
};

// 废弃转发：crop/拉直单参数（无旋转时 chain 恒等判定保证全幅 crop 字节不变）
class GeometryCropOp : public ParamOp {
 public:
  GeometryCropOp() : ParamOp("geometry.crop", Stage::S1Geometry) {}
  bool apply(imaging::ImageBuf* buf, std::string* err) const override {
    if (!checkBuf(buf, err)) return false;
    GeometryChainParams p;
    p.hasCrop = params_.get("geometry.hasCrop").asBool(false);
    p.cropLeft = params_.get("geometry.cropLeft").asDouble(0.0);
    p.cropTop = params_.get("geometry.cropTop").asDouble(0.0);
    p.cropRight = params_.get("geometry.cropRight").asDouble(100.0);
    p.cropBottom = params_.get("geometry.cropBottom").asDouble(100.0);
    p.cropAngle = params_.get("geometry.cropAngle").asDouble(0.0);
    return applyGeometryChain(buf, p, nullptr, err);
  }
};

// 废弃转发：透视/变换单参数（perspective 7 叶）
class GeometryPerspectiveOp : public ParamOp {
 public:
  GeometryPerspectiveOp() : ParamOp("geometry.perspective", Stage::S1Geometry) {}
  bool apply(imaging::ImageBuf* buf, std::string* err) const override {
    if (!checkBuf(buf, err)) return false;
    GeometryChainParams p;
    p.perspX = params_.get("geometry.perspective.x").asDouble(0.0);
    p.perspY = params_.get("geometry.perspective.y").asDouble(0.0);
    p.perspH = params_.get("geometry.perspective.horizontal").asDouble(0.0);
    p.perspV = params_.get("geometry.perspective.vertical").asDouble(0.0);
    p.perspRotate = params_.get("geometry.perspective.rotate").asDouble(0.0);
    p.perspScale = params_.get("geometry.perspective.scale").asDouble(1.0);
    p.perspAspect = params_.get("geometry.perspective.aspect").asDouble(0.0);
    return applyGeometryChain(buf, p, nullptr, err);
  }
};

}  // namespace

bool registerGeometryOps(OpTable* table, std::string* err) {
  bool ok = true;
  ok = table->registerOp(std::make_shared<GeometryApplyOp>(), err) && ok;
  ok = table->registerOp(std::make_shared<GeometryCropOp>(), err) && ok;
  ok = table->registerOp(std::make_shared<GeometryPerspectiveOp>(), err) && ok;
  return ok;
}

}  // namespace plan
}  // namespace lumina
