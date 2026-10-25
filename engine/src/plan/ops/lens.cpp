// lens节点算子骨架（S1 镜头 2 叶）。M1 T1.3 逐叶填实现。
#include "plan/ops/ops.h"

namespace lumina {
namespace plan {
namespace {

const OpSpec kOps[] = {
    {"lens.profile", Stage::S1Geometry},
    {"lens.manualDistortion", Stage::S1Geometry},
};

}  // namespace

bool registerLensOps(OpTable* table, std::string* err) {
  return registerBatch(table, kOps, sizeof(kOps) / sizeof(OpSpec), err);
}

}  // namespace plan
}  // namespace lumina
