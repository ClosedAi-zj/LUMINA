// T1.3 geometry 节点算子（13 叶，S1，CPU oracle）
// 裁剪/拉直 → plan::applyGeometry（T0.4 单应 + 一次采样）；透视 7 叶 → 同一单应。
// 参数键：geometry.hasCrop/cropLeft/cropTop/cropRight/cropBottom/cropAngle 与
// geometry.perspective.{x,y,horizontal,vertical,rotate,scale,aspect}。
#include "plan/ops/ops.h"

#include "plan/geometry.h"

#include <algorithm>

namespace lumina {
namespace plan {
namespace {

class GeometryCropOp : public ParamOp {
 public:
  GeometryCropOp() : ParamOp("geometry.crop", Stage::S1Geometry) {}
  bool apply(imaging::ImageBuf* buf, std::string* err) const override {
    if (!checkBuf(buf, err)) return false;
    const bool hasCrop = params_.get("geometry.hasCrop").asBool(false);
    const double angle = params_.get("geometry.cropAngle").asDouble(0.0);
    if (!hasCrop && angle == 0.0) return true;  // identity：无裁剪无拉直
    GeometryParams gp;
    gp.cropX = static_cast<int>(params_.get("geometry.cropLeft").asDouble(0.0) / 100.0 * buf->w);
    gp.cropY = static_cast<int>(params_.get("geometry.cropTop").asDouble(0.0) / 100.0 * buf->h);
    const double right = params_.get("geometry.cropRight").asDouble(100.0);
    const double bottom = params_.get("geometry.cropBottom").asDouble(100.0);
    gp.cropW = std::max(1, static_cast<int>(right / 100.0 * buf->w) - gp.cropX);
    gp.cropH = std::max(1, static_cast<int>(bottom / 100.0 * buf->h) - gp.cropY);
    gp.angleDeg = angle;
    gp.scale = 1.0;
    imaging::ImageBuf out;
    uint64_t samples = 0;
    if (!applyGeometry(*buf, gp, imaging::Kernel::CatmullRom, &out, &samples, err)) {
      return false;
    }
    if (samples != static_cast<uint64_t>(out.w) * out.h) {
      if (err) *err = "geometry.crop: sample count != dst pixels";
      return false;
    }
    *buf = std::move(out);
    return true;
  }
};

class GeometryPerspectiveOp : public ParamOp {
 public:
  GeometryPerspectiveOp() : ParamOp("geometry.perspective", Stage::S1Geometry) {}
  bool apply(imaging::ImageBuf* buf, std::string* err) const override {
    if (!checkBuf(buf, err)) return false;
    const double px = params_.get("geometry.perspective.x").asDouble(0.0);
    const double py = params_.get("geometry.perspective.y").asDouble(0.0);
    const double ph = params_.get("geometry.perspective.horizontal").asDouble(0.0);
    const double pv = params_.get("geometry.perspective.vertical").asDouble(0.0);
    const double rot = params_.get("geometry.perspective.rotate").asDouble(0.0);
    // scale 语义 = 倍数（UI 滑杆 0.5–2，protocol/TransformBar 默认 1=恒等）。
    // 曾按 100=恒等、/100 归一 → 会话默认 scale:1 被判激活且缩到 5%（T1.4 预览 4x3
    // 回归根因，2026-10-25 修复，与 app oracle93.buildGeomTransform 两端同步）。
    const double scale = params_.get("geometry.perspective.scale").asDouble(1.0);
    const double aspect = params_.get("geometry.perspective.aspect").asDouble(0.0);
    if (px == 0 && py == 0 && ph == 0 && pv == 0 && rot == 0 && scale == 1.0 && aspect == 0) {
      return true;  // identity
    }
    GeometryParams gp;
    gp.angleDeg = rot;
    gp.perspectiveH = ph / 100.0 + px / 100.0 * 0.1;
    gp.perspectiveV = pv / 100.0 + py / 100.0 * 0.1;
    gp.scale = std::max(0.05, scale);
    (void)aspect;  // 比例约束与裁剪框联动，T2.4 交互期实装（不静默改像素）
    imaging::ImageBuf out;
    uint64_t samples = 0;
    if (!applyGeometry(*buf, gp, imaging::Kernel::CatmullRom, &out, &samples, err)) {
      return false;
    }
    if (samples != static_cast<uint64_t>(out.w) * out.h) {
      if (err) *err = "geometry.perspective: sample count != dst pixels";
      return false;
    }
    *buf = std::move(out);
    return true;
  }
};

}  // namespace

bool registerGeometryOps(OpTable* table, std::string* err) {
  bool ok = true;
  ok = table->registerOp(std::make_shared<GeometryCropOp>(), err) && ok;
  ok = table->registerOp(std::make_shared<GeometryPerspectiveOp>(), err) && ok;
  return ok;
}

}  // namespace plan
}  // namespace lumina
