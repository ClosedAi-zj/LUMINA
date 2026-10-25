// calibration节点算子骨架（S2 校准 7 叶（tone map 之前））。M1 T1.3 逐叶填实现。
#include "plan/ops/ops.h"

namespace lumina {
namespace plan {
namespace {

const OpSpec kOps[] = {
    {"calibration.shadowTint", Stage::S2Tone},
    {"calibration.red", Stage::S2Tone},
    {"calibration.green", Stage::S2Tone},
    {"calibration.blue", Stage::S2Tone},
};

}  // namespace

bool registerCalibrationOps(OpTable* table, std::string* err) {
  return registerBatch(table, kOps, sizeof(kOps) / sizeof(OpSpec), err);
}

}  // namespace plan
}  // namespace lumina
