// geometry节点算子骨架（S1 几何 13 叶（裁剪/拉直/透视））。M1 T1.3 逐叶填实现。
#include "plan/ops/ops.h"

namespace lumina {
namespace plan {
namespace {

const OpSpec kOps[] = {
    {"geometry.crop", Stage::S1Geometry},
    {"geometry.perspective", Stage::S1Geometry},
};

}  // namespace

bool registerGeometryOps(OpTable* table, std::string* err) {
  return registerBatch(table, kOps, sizeof(kOps) / sizeof(OpSpec), err);
}

}  // namespace plan
}  // namespace lumina
