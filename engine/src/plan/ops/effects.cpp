// effects节点算子骨架（S6 裁后暗角 3 叶 + 颗粒 3 叶）。M1 T1.3 逐叶填实现。
#include "plan/ops/ops.h"

namespace lumina {
namespace plan {
namespace {

const OpSpec kOps[] = {
    {"effects.vignette", Stage::S6Effects},
    {"effects.grain", Stage::S6Effects},
};

}  // namespace

bool registerEffectsOps(OpTable* table, std::string* err) {
  return registerBatch(table, kOps, sizeof(kOps) / sizeof(OpSpec), err);
}

}  // namespace plan
}  // namespace lumina
