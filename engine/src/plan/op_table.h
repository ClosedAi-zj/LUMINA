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
