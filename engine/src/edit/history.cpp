// 撤销 / 重做栈实现 + T2.4 diff op 编解码（架构 §3.3.3）
#include "edit/history.h"

#include <algorithm>
#include <cstdlib>

namespace lumina {
namespace edit {

namespace {

// ---- op 路径解析："masks[0].local.exposure" → [{masks,0},{local,-1},{exposure,-1}] ----
struct PathSeg {
  std::string key;
  int index = -1;  // -1 = 纯对象键；>=0 = key 指向的数组再按下标取
};

bool parsePath(const std::string& path, std::vector<PathSeg>* segs) {
  size_t pos = 0;
  while (pos < path.size()) {
    const size_t dot = path.find('.', pos);
    const std::string token =
        path.substr(pos, (dot == std::string::npos ? path.size() : dot) - pos);
    pos = (dot == std::string::npos) ? path.size() : dot + 1;
    if (token.empty()) return false;
    PathSeg seg;
    const size_t br = token.find('[');
    if (br == std::string::npos) {
      seg.key = token;
    } else {
      seg.key = token.substr(0, br);
      const size_t close = token.find(']', br);
      if (close == std::string::npos) return false;
      const std::string num = token.substr(br + 1, close - br - 1);
      if (num.empty()) return false;
      char* endp = nullptr;
      const long idx = std::strtol(num.c_str(), &endp, 10);
      if (endp == nullptr || *endp != '\0' || idx < 0) return false;
      // 只支持一层下标（当前参数形态没有嵌套数组下标）
      if (token.find('[', close) != std::string::npos) return false;
      seg.index = static_cast<int>(idx);
    }
    segs->push_back(seg);
  }
  return !segs->empty();
}

// 函数式路径写入：逐段返回新节点（按值拷贝，避免容器指针失效），叶节点写入 leaf
bool setPathSeg(const json::JsonValue& node, size_t i, const std::vector<PathSeg>& segs,
                const json::JsonValue& leaf, json::JsonValue* out) {
  const PathSeg& seg = segs[i];
  const bool last = (i + 1 == segs.size());

  if (seg.key.empty()) {
    // 纯数组段（路径以 [n] 开头；当前参数形态用不到，防御性支持）
    std::vector<json::JsonValue> items =
        node.isArray() ? node.arrayItems() : std::vector<json::JsonValue>();
    while (static_cast<int>(items.size()) <= seg.index) {
      items.push_back(json::JsonValue::nullValue());
    }
    json::JsonValue element = items[seg.index];
    json::JsonValue newElement;
    if (last) {
      newElement = leaf;
    } else if (!setPathSeg(element, i + 1, segs, leaf, &newElement)) {
      return false;
    }
    items[seg.index] = newElement;
    json::JsonValue arr = json::JsonValue::arrayValue();
    for (const auto& it : items) arr.addItem(it);
    *out = arr;
    return true;
  }

  if (seg.index < 0) {
    // 纯对象键
    const json::JsonValue child =
        node.contains(seg.key) ? node.get(seg.key) : json::JsonValue::nullValue();
    json::JsonValue newChild;
    if (last) {
      newChild = leaf;
    } else if (!setPathSeg(child, i + 1, segs, leaf, &newChild)) {
      return false;
    }
    *out = node;
    out->setItem(seg.key, newChild);
    return true;
  }

  // key + [index]：node 是对象，node[key] 是数组（不存在则补建空数组）
  const json::JsonValue child =
      node.contains(seg.key) ? node.get(seg.key) : json::JsonValue::nullValue();
  std::vector<json::JsonValue> items =
      child.isArray() ? child.arrayItems() : std::vector<json::JsonValue>();
  while (static_cast<int>(items.size()) <= seg.index) {
    items.push_back(json::JsonValue::nullValue());
  }
  json::JsonValue element = items[seg.index];
  json::JsonValue newElement;
  if (last) {
    newElement = leaf;
  } else if (!setPathSeg(element, i + 1, segs, leaf, &newElement)) {
    return false;
  }
  items[seg.index] = newElement;
  json::JsonValue arr = json::JsonValue::arrayValue();
  for (const auto& it : items) arr.addItem(it);
  *out = node;
  out->setItem(seg.key, arr);
  return true;
}

// 递归收集差异：对象按键并集下钻；同长数组按下标下钻（路径记 [i]）；其余按序列化比对
void collect(const std::string& path, const json::JsonValue& a, const json::JsonValue& b,
             std::vector<json::JsonValue>* ops) {
  if (a.isObject() && b.isObject()) {
    for (const auto& kv : b.objectItems()) {
      const std::string child = path.empty() ? kv.first : path + "." + kv.first;
      if (a.contains(kv.first)) {
        collect(child, a.get(kv.first), kv.second, ops);
      } else {
        collect(child, json::JsonValue::nullValue(), kv.second, ops);  // 新增
      }
    }
    for (const auto& kv : a.objectItems()) {
      if (!b.contains(kv.first)) {
        const std::string child = path.empty() ? kv.first : path + "." + kv.first;
        collect(child, kv.second, json::JsonValue::nullValue(), ops);  // 删除
      }
    }
    return;
  }
  if (a.isArray() && b.isArray() && a.size() == b.size()) {
    for (size_t i = 0; i < a.size(); ++i) {
      collect(path + "[" + std::to_string(i) + "]", a.at(i), b.at(i), ops);
    }
    return;
  }
  if (a.serialize() != b.serialize()) {
    json::JsonValue op = json::JsonValue::objectValue();
    op.setItem("t", "param");
    op.setItem("path", path);
    op.setItem("from", a);
    op.setItem("to", b);
    ops->push_back(op);
  }
}

// UI 显示名用的短值（超长截断）
std::string shortVal(const json::JsonValue& v) {
  std::string s = v.serialize();
  if (s.size() > 40) s = s.substr(0, 37) + "...";
  return s;
}

std::string labelForOp(const json::JsonValue& op) {
  if (!op.isObject()) return std::string();
  if (op.get("t").asString() == "bulk") {
    return "批量修改 " + std::to_string(op.get("ops").size()) + " 项";
  }
  return op.get("path").asString() + ": " + shortVal(op.get("from")) + " → " +
         shortVal(op.get("to"));
}

// 单个 op 应用（param / bulk）；未知类型显式失败
bool applyOneOp(const json::JsonValue& cur, const json::JsonValue& op, json::JsonValue* out) {
  if (!op.isObject()) return false;
  const std::string t = op.get("t").asString();
  if (t == "param") {
    std::vector<PathSeg> segs;
    if (!parsePath(op.get("path").asString(), &segs)) return false;
    return setPathSeg(cur, 0, segs, op.get("to"), out);
  }
  if (t == "bulk") {
    const json::JsonValue& sub = op.get("ops");
    if (!sub.isArray()) return false;
    json::JsonValue node = cur;
    for (const auto& one : sub.arrayItems()) {
      json::JsonValue next;
      if (!applyOneOp(node, one, &next)) return false;
      node = next;
    }
    *out = node;
    return true;
  }
  // mask/node 等 op 由蒙版引擎（T2.6）接入时扩展；未知类型显式失败，禁止静默半应用
  return false;
}

}  // namespace

// ---------------- HistoryStack ----------------

void HistoryStack::push(const json::JsonValue& snapshot) {
  pushCommitted(snapshot, json::JsonValue::nullValue());
}

void HistoryStack::pushCommitted(const json::JsonValue& snapshot, const json::JsonValue& op) {
  if (index_ + 1 < static_cast<int>(entries_.size())) {
    entries_.erase(entries_.begin() + index_ + 1, entries_.end());
    ops_.erase(ops_.begin() + index_ + 1, ops_.end());
  }
  entries_.push_back(snapshot);
  ops_.push_back(op);
  index_ = static_cast<int>(entries_.size()) - 1;
  // 单图 500 步容量：丢最老步（被丢的步转入虚拟历史，totalSteps 口径不变）
  if (static_cast<int>(entries_.size()) > kHistoryMaxSteps) {
    entries_.erase(entries_.begin());
    ops_.erase(ops_.begin());
    --index_;
    ++virtualBase_;
  }
}

void HistoryStack::replaceCurrent(const json::JsonValue& snapshot) {
  if (index_ < 0 || index_ >= static_cast<int>(entries_.size())) {
    push(snapshot);
    return;
  }
  entries_[index_] = snapshot;
}

const json::JsonValue& HistoryStack::current() const {
  static const json::JsonValue kNull = json::JsonValue::nullValue();
  if (index_ < 0 || index_ >= static_cast<int>(entries_.size())) return kNull;
  return entries_[index_];
}

const json::JsonValue& HistoryStack::opAt(int i) const {
  static const json::JsonValue kNull = json::JsonValue::nullValue();
  if (i < 0 || i >= static_cast<int>(ops_.size())) return kNull;
  return ops_[i];
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

bool HistoryStack::restoreFrom(const json::JsonValue& base, const json::JsonValue& ops,
                               int targetIndex) {
  if (!ops.isArray() || targetIndex < 0) return false;
  clear();
  push(base);
  const auto& items = ops.arrayItems();
  const int limit = std::min<int>(targetIndex, static_cast<int>(items.size()));
  for (int i = 0; i < limit; ++i) {
    json::JsonValue next;
    if (!applyOneOp(entries_.back(), items[i], &next)) {
      clear();  // 恢复失败不留半重放栈，调用方显式重置
      return false;
    }
    pushCommitted(next, items[i]);
  }
  // 虚拟前缀 = 已重放步数中被 500 上限裁掉的部分（正常远小于上限，此式对裁剪也自洽）
  virtualBase_ = targetIndex - (static_cast<int>(entries_.size()) - 1);
  if (virtualBase_ < 0) virtualBase_ = 0;
  return true;
}

void HistoryStack::clear() {
  entries_.clear();
  ops_.clear();
  index_ = -1;
  virtualBase_ = 0;
}

// ---------------- diff op 编解码 ----------------

json::JsonValue diffOps(const json::JsonValue& prev, const json::JsonValue& next,
                        std::string* labelOut) {
  std::vector<json::JsonValue> ops;
  collect("", prev, next, &ops);
  if (ops.empty()) return json::JsonValue::nullValue();
  if (ops.size() == 1) {
    if (labelOut) *labelOut = labelForOp(ops[0]);
    return ops[0];
  }
  json::JsonValue bulk = json::JsonValue::objectValue();
  bulk.setItem("t", "bulk");
  json::JsonValue arr = json::JsonValue::arrayValue();
  for (const auto& op : ops) arr.addItem(op);
  bulk.setItem("ops", arr);
  if (labelOut) *labelOut = labelForOp(bulk);
  return bulk;
}

bool applyOps(const json::JsonValue& base, const json::JsonValue& ops, json::JsonValue* out) {
  if (!ops.isArray()) return false;
  json::JsonValue cur = base;
  for (const auto& op : ops.arrayItems()) {
    json::JsonValue next;
    if (!applyOneOp(cur, op, &next)) return false;
    cur = next;
  }
  *out = cur;
  return true;
}

}  // namespace edit
}  // namespace lumina
