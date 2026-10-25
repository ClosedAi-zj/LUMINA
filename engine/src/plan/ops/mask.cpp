// mask节点算子骨架（蒙版 1 叶（随宿主节点；骨架挂 S4））。M1 T1.3 逐叶填实现。
#include "plan/ops/ops.h"

namespace lumina {
namespace plan {
namespace {

const OpSpec kOps[] = {
    {"mask.apply", Stage::S4Color},
};

}  // namespace

bool registerMaskOps(OpTable* table, std::string* err) {
  return registerBatch(table, kOps, sizeof(kOps) / sizeof(OpSpec), err);
}

}  // namespace plan
}  // namespace lumina
