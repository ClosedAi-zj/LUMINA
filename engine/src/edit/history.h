// 撤销 / 重做栈：EditParams 快照（commitHistory=false 的滑块中间态不入栈）
#ifndef LUMINA_EDIT_HISTORY_H_
#define LUMINA_EDIT_HISTORY_H_

#include <vector>

#include "json/json_value.h"

namespace lumina {
namespace edit {

class HistoryStack {
 public:
  // 压入一次已提交的状态（会清掉 redo 分支）
  void push(const json::JsonValue& snapshot);

  // 覆盖当前栈顶（滑块拖动中间态：不入新历史）
  void replaceCurrent(const json::JsonValue& snapshot);

  bool canUndo() const { return index_ > 0; }
  bool canRedo() const { return index_ + 1 < static_cast<int>(entries_.size()); }
  int index() const { return index_; }
  int length() const { return static_cast<int>(entries_.size()); }

  // 取当前状态
  const json::JsonValue& current() const;

  // 撤销：索引前移并返回该状态
  bool undo(json::JsonValue* out);
  bool redo(json::JsonValue* out);

  void clear() {
    entries_.clear();
    index_ = -1;
  }

 private:
  std::vector<json::JsonValue> entries_;
  int index_ = -1;
};

}  // namespace edit
}  // namespace lumina

#endif  // LUMINA_EDIT_HISTORY_H_
