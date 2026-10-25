// curve节点算子骨架（S4 曲线 11 叶）。M1 T1.3 逐叶填实现。
#include "plan/ops/ops.h"

namespace lumina {
namespace plan {
namespace {

const OpSpec kOps[] = {
    {"curve.toneCurveRGB", Stage::S4Color},
    {"curve.toneCurveR", Stage::S4Color},
    {"curve.toneCurveG", Stage::S4Color},
    {"curve.toneCurveB", Stage::S4Color},
    {"curve.parametric.highlights", Stage::S4Color},
    {"curve.parametric.lights", Stage::S4Color},
    {"curve.parametric.darks", Stage::S4Color},
    {"curve.parametric.shadows", Stage::S4Color},
    {"curve.parametric.shadowSplit", Stage::S4Color},
    {"curve.parametric.lightSplit", Stage::S4Color},
    {"curve.parametric.midSplit", Stage::S4Color},
};

}  // namespace

bool registerCurveOps(OpTable* table, std::string* err) {
  return registerBatch(table, kOps, sizeof(kOps) / sizeof(OpSpec), err);
}

}  // namespace plan
}  // namespace lumina
