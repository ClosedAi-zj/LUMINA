// 算子表：stage 有序求值 + CPU oracle 入口（架构 §4/§5.2，M0 T0.3 骨架）
// 纪律：算子输入/输出均为 ImageBuf（log16 载体）；stage 错序 = 运行期报错；
//       M1 的 93 叶算子全部落在本表（plan/ops/*.cpp）。
#ifndef LUMINA_PLAN_OP_TABLE_H_
#define LUMINA_PLAN_OP_TABLE_H_

#include <map>
#include <memory>
#include "json/json_value.h"
#include <string>
#include <vector>

#include "imaging/image_buffer.h"

namespace lumina {
namespace plan {

// 求值阶段（严格有序，禁止逆序调用）。S2 与 S3 之间是唯一域分界：
// S3 的 ToneMap 之后进入 display-referred 语义。
enum class Stage {
  S0Decode = 0,     // 解码 + 白平衡通道增益（scene-linear camera）
  S1Geometry,       // 裁剪/拉直/透视（一次重采样）
  S15Denoise,       // 降噪（NR 先于锐化）
  S2Tone,           // 曝光/黑白场等线性光算子 + calibration（tone map 之前）
  S3ToneMap,        // scene-linear → [0,1] 显示域（唯一域分界，算子非节点）
  S4Color,          // 曲线 / HSL / 颜色分级（感知域）
  S5Detail,         // 锐化（最后，避免振铃被后续算子放大）
  S6Effects,        // 裁后暗角 / 颗粒
  S7Output,         // 输出编码（8bit TPDF / 16bit 线性）
  kStageCount,
};

// stage 序号（供校验与缓存键）
inline int stageIndex(Stage s) { return static_cast<int>(s); }
// a 是否允许排在 b 之前（同 stage 可重复）
inline bool stageAllowedAfter(Stage a, Stage current) {
  return stageIndex(a) >= stageIndex(current);
}
const char* stageName(Stage s);

// 算子接口：CPU oracle 的统一入口（GPU 侧在 M1 只允许逼近 CPU 结果）
class Operator {
 public:
  virtual ~Operator() = default;
  virtual const char* name() const = 0;
  virtual Stage stage() const = 0;
  // 就地变换 buf；错误（如域不匹配）写 err 并返回 false。零静默。
  virtual bool apply(imaging::ImageBuf* buf, std::string* err) const = 0;
  // T1.3：求值前注入本算子参数（键 = 叶路径口径，如 "exposure"/"hsl.blue.sat"）。
  // 默认忽略（无参算子）。evaluate(带 params) 会对序列中每个算子调用。
  virtual void setParams(const json::JsonValue& params) { (void)params; }
};

// ---- 20 阶段顺序表（M2 T2.1 / 架构 §3.7）：唯一事实源 ----
// 用途：O1 断言（kPipelineOrder 行序 + Plan opSequence 的 stage 单调不减 +
//       opSequence 与算子级行构成子序列）；也是评审「顺序是否被改动」的比对基准。
// 纪律（S10）：本表 CI 只读，开发侧不得重排、不得删行；只许在尾部追加新阶段
//              （追加即改 kPipelineOrderCount，属口径变更，需先改架构文档）。
// 域（S2）：S0–S2 = scene-linear log16 码域；S3 = 唯一域分界；S4–S6 = 感知码域。
struct PipelineStageDesc {
  int index = 0;          // 1..20（行号，O1 断言的稳定标识）
  const char* label = "";  // 中文阶段名（日志/失败信息可读）
  Stage stage = Stage::S0Decode;
  // 本行覆盖的算子名，逗号分隔（算子级行的"构成子序列"断言按此匹配）。
  // 空串 = 本行没有独立算子（全部落在 geometry.apply 的子步里）。
  const char* ops = "";
  // geometry.apply 内部子步号（1..6）；0 = 独立算子行。
  // 子步顺序由 tests/test_geometry_chain.cpp 单独断言（T2.10），本表只登记。
  int subStep = 0;
  // 一行的结束 stage（仅第 20 行跨 S6Effects→S7Output；其余与 stage 相同）
  Stage stageEnd = Stage::S0Decode;
  const char* domain = "";  // 该阶段的像素域说明
};

// 行数常量（= 20）。O1 断言 `kPipelineOrderCount == 20`。
constexpr int kPipelineOrderCount = 20;
extern const PipelineStageDesc kPipelineOrder[kPipelineOrderCount];

// 算子名 → 所在行号（1..20）；未登记返回 0。
// T2.10 收口（§3.7.1）：S1 已合并为单一算子 geometry.apply（登记在第 2 行 ops），
// 旧算子名（lens.profile / lens.manualDistortion / lens.chromaticAberration /
// geometry.crop / geometry.perspective）的兼容映射已删除——它们不再被 Plan 产出，
// 查询返回 0（未登记）。
int pipelineRowForOp(const std::string& opName);

// T2.10 表驱动（§3.7.1 第 1/2 条）：rank 由 kPipelineOrder 派生 =
// 行号 * 100 + 行内算子下标（单一事实源，执行侧 compilePlan 只认本函数）。
// 未登记算子 → 写 err 并返回 false（硬失败，**无兜底值**——S3 零静默，
// 删除了旧 semanticRank 的 return 1000 兜底）。
// 例外白名单：iccInput（M4 实装时正式登记进表，暂归第 15/16 行界）。
bool pipelineRankForOp(const std::string& opName, int* rankOut, std::string* err);

// 算子注册表 + 有序求值器（骨架；M0 期末由 Plan 求值器接管 evaluate）
class OpTable {
 public:
  static OpTable& instance();

  // 注册；重名或非法 stage 拒绝并写 err
  bool registerOp(std::shared_ptr<Operator> op, std::string* err);
  const Operator* find(const std::string& name) const;
  size_t size() const { return ops_.size(); }

  // 按给定顺序求值。校验：
  // ① 未知算子 → 报错；② stage 逆序（op.stage() < 当前已到达 stage）→ 报错；
  // ③ 单算子 apply 失败 → 透传报错。任何失败不落盘、不部分求值（调用方丢弃 buf）。
  bool evaluate(const std::vector<std::string>& opNames, imaging::ImageBuf* buf,
                std::string* err) const;
  // T1.3：带参数求值（opName → params 叶路径键值集）。非 const（注入 setParams）。
  bool evaluate(const std::vector<std::string>& opNames, imaging::ImageBuf* buf,
                const std::map<std::string, json::JsonValue>& paramsByOp,
                std::string* err);

 private:
  OpTable() = default;
  std::map<std::string, std::shared_ptr<Operator>> ops_;
};

// 骨架算子注册入口（M1 逐叶填充实现）。任一注册失败返回 false（err 写明原因）。
bool registerBuiltinOps(OpTable* table, std::string* err);

}  // namespace plan
}  // namespace lumina

#endif  // LUMINA_PLAN_OP_TABLE_H_
