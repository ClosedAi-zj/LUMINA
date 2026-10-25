// color节点算子骨架（S4 HSL 8 带×3 + 颜色分级 8 = 32 叶）。M1 T1.3 逐叶填实现。
#include "plan/ops/ops.h"

namespace lumina {
namespace plan {
namespace {

const OpSpec kOps[] = {
    {"color.hsl", Stage::S4Color},
    {"color.colorGrade", Stage::S4Color},
};

}  // namespace

bool registerColorOps(OpTable* table, std::string* err) {
  return registerBatch(table, kOps, sizeof(kOps) / sizeof(OpSpec), err);
}

}  // namespace plan
}  // namespace lumina
