// 撤销 / 重做栈 + T2.4 diff op 编解码（EditParams 快照；commitHistory=false 的滑块中间态不入栈）
#ifndef LUMINA_EDIT_HISTORY_H_
#define LUMINA_EDIT_HISTORY_H_

#include <string>
#include <vector>

#include "json/json_value.h"

namespace lumina {
namespace edit {

// 单图历史步数上限（架构 §3.3.3「单图 500 步」；内存栈超限丢最老步，持久化由
// HistoryDao::kFoldEvery 折叠控制容量）
constexpr int kHistoryMaxSteps = 500;

class HistoryStack {
 public:
  // 压入一次已提交的状态（会清掉 redo 分支；超过 kHistoryMaxSteps 时丢最老步）
  void push(const json::JsonValue& snapshot);

  // T2.4：带 diff op 的压入（op = prev→snapshot 的变更编码，见 diffOps；op 为 null
  // 表示无差异提交，只入内存栈不产生持久化 op）。base 首条也走本入口（op=null）。
  void pushCommitted(const json::JsonValue& snapshot, const json::JsonValue& op);

  // 覆盖当前栈顶（滑块拖动中间态：不入新历史）
  void replaceCurrent(const json::JsonValue& snapshot);

  bool canUndo() const { return index_ > 0; }
  bool canRedo() const { return index_ + 1 < static_cast<int>(entries_.size()); }
  int index() const { return index_; }
  int length() const { return static_cast<int>(entries_.size()); }

  // entries_[0] 之前的虚拟历史步数（跨会话折叠恢复后 >0；UI「首条灰字·上次会话」
  // 即由此而来）。持久化游标口径：current_seq = virtualBase() + index()。
  int virtualBase() const { return virtualBase_; }
  // 持久化 historyLength 口径 = current_seq + 1
  int totalSteps() const { return virtualBase_ + length(); }

  // 取当前状态
  const json::JsonValue& current() const;

  // 第 i 步的 diff op（entries_[i-1]→entries_[i]；i==0 返回 null）
  const json::JsonValue& opAt(int i) const;

  // 撤销：索引前移并返回该状态
  bool undo(json::JsonValue* out);
  bool redo(json::JsonValue* out);

  // T2.4 跨会话恢复：base（最近 checkpoint 的完整参数）+ 有序 diff op 重放到
  // targetIndex（= 持久化 current_seq）。失败（未知 op / 损坏路径）返回 false 且
  // 栈保持空——调用方必须显式重置（零静默）。
  bool restoreFrom(const json::JsonValue& base, const json::JsonValue& ops, int targetIndex);

  void clear();

 private:
  std::vector<json::JsonValue> entries_;
  std::vector<json::JsonValue> ops_;  // ops_[i] 与 entries_[i] 对齐；ops_[0] 恒为 null
  int index_ = -1;
  int virtualBase_ = 0;
};

// ---- T2.4 diff op（架构 §3.3.3「op 编码」）----
// 计算 prev→next 的变更 op：单条 {"t":"param","path","from","to"}（路径形如
// "exposure" / "masks[0].local.exposure"）；多条变更打包 {"t":"bulk","ops":[...]}；
// 无差异返回 null。labelOut（可空）生成 UI 显示名。
// 说明：架构示例中的 "node" 字段在本引擎参数形态下由 path 承载（节点参数位于
// nodes[i] 数组内），不单独建字段——编码语义不变。
json::JsonValue diffOps(const json::JsonValue& prev, const json::JsonValue& next,
                        std::string* labelOut);

// 将有序 op 列表（每项为 diffOps 的产物：param 或 bulk）依次应用到 base。
// 未知 op 类型 / 损坏路径 → false（调用方必须显式重置历史，禁止静默半应用）。
bool applyOps(const json::JsonValue& base, const json::JsonValue& ops, json::JsonValue* out);

}  // namespace edit
}  // namespace lumina

#endif  // LUMINA_EDIT_HISTORY_H_
