// detail节点算子骨架（S1.5 NR 3 叶 + S5 锐化 4 叶）。M1 T1.3 逐叶填实现。
#include "plan/ops/ops.h"

namespace lumina {
namespace plan {
namespace {

const OpSpec kOps[] = {
    {"detail.denoise", Stage::S15Denoise},
    {"detail.sharpen", Stage::S5Detail},
};

}  // namespace

bool registerDetailOps(OpTable* table, std::string* err) {
  return registerBatch(table, kOps, sizeof(kOps) / sizeof(OpSpec), err);
}

}  // namespace plan
}  // namespace lumina
