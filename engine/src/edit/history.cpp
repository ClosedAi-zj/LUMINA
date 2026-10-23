// 撤销 / 重做栈实现
#include "edit/history.h"

namespace lumina {
namespace edit {

void HistoryStack::push(const json::JsonValue& snapshot) {
  if (index_ + 1 < static_cast<int>(entries_.size())) {
    entries_.erase(entries_.begin() + index_ + 1, entries_.end());
  }
  entries_.push_back(snapshot);
  index_ = static_cast<int>(entries_.size()) - 1;
}

void HistoryStack::replaceCurrent(const json::JsonValue& snapshot) {
  if (index_ < 0 || index_ >= static_cast<int>(entries_.size())) {
    entries_.push_back(snapshot);
    index_ = static_cast<int>(entries_.size()) - 1;
    return;
  }
  entries_[index_] = snapshot;
}

const json::JsonValue& HistoryStack::current() const {
  static const json::JsonValue kNull = json::JsonValue::nullValue();
  if (index_ < 0 || index_ >= static_cast<int>(entries_.size())) return kNull;
  return entries_[index_];
}

bool HistoryStack::undo(json::JsonValue* out) {
  if (!canUndo()) return false;
  --index_;
  *out = entries_[index_];
  return true;
}

bool HistoryStack::redo(json::JsonValue* out) {
  if (!canRedo()) return false;
  ++index_;
  *out = entries_[index_];
  return true;
}

}  // namespace edit
}  // namespace lumina
