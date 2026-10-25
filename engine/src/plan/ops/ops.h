// M0 T0.3 算子骨架：8 节点占位算子（M1 T1.3 逐叶填实现）
// 每个文件一个注册入口；骨架期语义 = identity（不改像素），stage/域标签先行落对。
#ifndef LUMINA_PLAN_OPS_OPS_H_
#define LUMINA_PLAN_OPS_OPS_H_

#include "plan/op_table.h"

namespace lumina {
namespace plan {

bool registerBasicOps(OpTable* table, std::string* err);       // S0 WB / S2 基础色调（14 叶）
bool registerCurveOps(OpTable* table, std::string* err);       // S4 曲线（11 叶）
bool registerColorOps(OpTable* table, std::string* err);       // S4 HSL + 颜色分级（32 叶）
bool registerCalibrationOps(OpTable* table, std::string* err); // S2 校准（7 叶）
bool registerDetailOps(OpTable* table, std::string* err);      // S1.5 NR / S5 锐化（7 叶）
bool registerLensOps(OpTable* table, std::string* err);        // S1 镜头（2 叶）
bool registerGeometryOps(OpTable* table, std::string* err);    // S1 几何（13 叶）
bool registerEffectsOps(OpTable* table, std::string* err);     // S6 暗角/颗粒（6 叶）
bool registerMaskOps(OpTable* table, std::string* err);        // 随宿主节点的蒙版（1 叶）

// ---- 骨架期共享辅助（M1 各节点拆真实现后可删） ----

// identity 占位算子：stage/域标签先行落对，apply 不改像素
class IdentityOp : public Operator {
 public:
  IdentityOp(const char* n, Stage s) : n_(n), s_(s) {}
  const char* name() const override { return n_; }
  Stage stage() const override { return s_; }
  bool apply(imaging::ImageBuf* buf, std::string* err) const override {
    if (!buf || !buf->valid()) {
      if (err) *err = std::string(n_) + ": invalid buffer";
      return false;
    }
    return true;  // identity：骨架期不改动像素
  }
 private:
  const char* n_;
  Stage s_;
};

struct OpSpec {
  const char* name;
  Stage stage;
};
inline bool registerBatch(OpTable* table, const OpSpec* specs, size_t n, std::string* err) {
  bool ok = true;
  for (size_t i = 0; i < n; ++i)
    ok = table->registerOp(std::make_shared<IdentityOp>(specs[i].name, specs[i].stage), err) && ok;
  return ok;
}

}  // namespace plan
}  // namespace lumina

#endif  // LUMINA_PLAN_OPS_OPS_H_
