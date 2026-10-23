// 自研最小 JSON 模块：JSON-RPC 协议专用值对象 + 解析器 + 序列化器（保序 object）
#ifndef LUMINA_JSON_JSON_VALUE_H_
#define LUMINA_JSON_JSON_VALUE_H_

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace lumina {
namespace json {

enum class Type { kNull, kBool, kInt, kDouble, kString, kArray, kObject };

class JsonValue {
 public:
  using ObjectItem = std::pair<std::string, JsonValue>;

  JsonValue();

  // ---- 工厂 ----
  static JsonValue nullValue();
  static JsonValue boolean(bool v);
  static JsonValue integer(int64_t v);
  static JsonValue number(double v);
  static JsonValue str(const std::string& v);
  static JsonValue str(const char* v);
  static JsonValue arrayValue();
  static JsonValue objectValue();

  // ---- 类型查询 ----
  Type type() const { return type_; }
  bool isNull() const { return type_ == Type::kNull; }
  bool isBool() const { return type_ == Type::kBool; }
  bool isInt() const { return type_ == Type::kInt; }
  bool isDouble() const { return type_ == Type::kDouble; }
  bool isNumber() const { return isInt() || isDouble(); }
  bool isString() const { return type_ == Type::kString; }
  bool isArray() const { return type_ == Type::kArray; }
  bool isObject() const { return type_ == Type::kObject; }

  // ---- 取值（带默认值，永不抛异常） ----
  bool asBool(bool def = false) const;
  int64_t asInt(int64_t def = 0) const;
  double asDouble(double def = 0.0) const;
  std::string asString(const std::string& def = std::string()) const;
  const std::string& stringRef() const;  // 仅 isString 时有意义，否则返回空串常量
  size_t size() const;                   // 数组长度 / 对象键数
  bool empty() const { return size() == 0; }

  // ---- 对象访问 ----
  bool contains(const std::string& key) const;
  const JsonValue& get(const std::string& key) const;  // 缺失返回全局 kNull
  void setItem(const std::string& key, const JsonValue& v);
  void setItem(const std::string& key, const std::string& v);
  void setItem(const std::string& key, const char* v);
  void setItem(const std::string& key, int64_t v);
  void setItem(const std::string& key, int v);
  void setItem(const std::string& key, double v);
  void setItem(const std::string& key, bool v);
  bool eraseKey(const std::string& key);
  const std::vector<ObjectItem>& objectItems() const { return object_; }

  // ---- 数组访问 ----
  const JsonValue& at(size_t index) const;  // 越界返回全局 kNull
  void addItem(const JsonValue& v);
  void addItem(const std::string& v);
  void addItem(const char* v);
  void addItem(int64_t v);
  void addItem(int v);
  void addItem(double v);
  void addItem(bool v);
  const std::vector<JsonValue>& arrayItems() const { return array_; }

  // ---- 解析 / 序列化 ----
  static bool parse(const std::string& text, JsonValue* out, std::string* err);
  std::string serialize() const;
  // 紧凑序列化（同上，协议层只用紧凑形式）
  std::string dump() const { return serialize(); }

  // ---- 深合并（对象递归；非对象整体覆盖）——用于 EditParams patch ----
  void mergePatch(const JsonValue& patch);

  // 全局空值，供 get()/at() 返回引用
  static const JsonValue& nullRef();

 private:
  void serializeTo(std::string* out) const;

  Type type_;
  bool bool_;
  int64_t int_;
  double double_;
  std::string string_;
  std::vector<JsonValue> array_;
  std::vector<ObjectItem> object_;
};

}  // namespace json
}  // namespace lumina

#endif  // LUMINA_JSON_JSON_VALUE_H_
