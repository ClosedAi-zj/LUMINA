// 自研最小 JSON 模块实现：递归下降解析 + 紧凑序列化（UTF-8 直通）
#include "json/json_value.h"

#include <cmath>
#include <cstdio>
#include <cstring>

namespace lumina {
namespace json {
namespace {

// 解析器：对 UTF-8 文本做递归下降，不做任何内存拷贝优化（协议帧 <8MiB，够用）
class Parser {
 public:
  Parser(const char* p, const char* end) : p_(p), end_(end) {}

  bool parseValue(JsonValue* out, std::string* err) {
    skipWs();
    if (p_ >= end_) return fail("unexpected end of input", err);
    char c = *p_;
    switch (c) {
      case '{': return parseObject(out, err);
      case '[': return parseArray(out, err);
      case '"': {
        std::string s;
        if (!parseString(&s, err)) return false;
        *out = JsonValue::str(s);
        return true;
      }
      case 't':
        if (match("true")) { *out = JsonValue::boolean(true); return true; }
        return fail("invalid literal", err);
      case 'f':
        if (match("false")) { *out = JsonValue::boolean(false); return true; }
        return fail("invalid literal", err);
      case 'n':
        if (match("null")) { *out = JsonValue::nullValue(); return true; }
        return fail("invalid literal", err);
      default:
        return parseNumber(out, err);
    }
  }

 private:
  void skipWs() {
    while (p_ < end_ && (*p_ == ' ' || *p_ == '\t' || *p_ == '\n' || *p_ == '\r')) ++p_;
  }

  bool match(const char* lit) {
    size_t n = std::strlen(lit);
    if (static_cast<size_t>(end_ - p_) < n) return false;
    if (std::memcmp(p_, lit, n) != 0) return false;
    p_ += n;
    return true;
  }

  bool fail(const char* msg, std::string* err) {
    if (err) {
      size_t off = 0;
      *err = std::string(msg) + " at offset " + std::to_string(off);
    }
    return false;
  }

  bool parseObject(JsonValue* out, std::string* err) {
    ++p_;  // '{'
    *out = JsonValue::objectValue();
    skipWs();
    if (p_ < end_ && *p_ == '}') { ++p_; return true; }
    while (p_ < end_) {
      skipWs();
      if (*p_ != '"') return fail("expected key string", err);
      std::string key;
      if (!parseString(&key, err)) return false;
      skipWs();
      if (p_ >= end_ || *p_ != ':') return fail("expected ':'", err);
      ++p_;
      JsonValue v;
      if (!parseValue(&v, err)) return false;
      out->setItem(key, v);
      skipWs();
      if (p_ < end_ && *p_ == ',') { ++p_; continue; }
      if (p_ < end_ && *p_ == '}') { ++p_; return true; }
      return fail("expected ',' or '}'", err);
    }
    return fail("unterminated object", err);
  }

  bool parseArray(JsonValue* out, std::string* err) {
    ++p_;  // '['
    *out = JsonValue::arrayValue();
    skipWs();
    if (p_ < end_ && *p_ == ']') { ++p_; return true; }
    while (p_ < end_) {
      JsonValue v;
      if (!parseValue(&v, err)) return false;
      out->addItem(v);
      skipWs();
      if (p_ < end_ && *p_ == ',') { ++p_; continue; }
      if (p_ < end_ && *p_ == ']') { ++p_; return true; }
      return fail("expected ',' or ']'", err);
    }
    return fail("unterminated array", err);
  }

  // 解析字符串；支持 \" \\ \/ \b \f \n \r \t \uXXXX（含代理对 → UTF-8）
  bool parseString(std::string* out, std::string* err) {
    out->clear();
    ++p_;  // '"'
    while (p_ < end_) {
      char c = *p_;
      if (c == '"') { ++p_; return true; }
      if (c == '\\') {
        ++p_;
        if (p_ >= end_) return fail("bad escape", err);
        char e = *p_++;
        switch (e) {
          case '"': out->push_back('"'); break;
          case '\\': out->push_back('\\'); break;
          case '/': out->push_back('/'); break;
          case 'b': out->push_back('\b'); break;
          case 'f': out->push_back('\f'); break;
          case 'n': out->push_back('\n'); break;
          case 'r': out->push_back('\r'); break;
          case 't': out->push_back('\t'); break;
          case 'u': {
            uint32_t cp = 0;
            if (!parseHex4(&cp, err)) return false;
            if (cp >= 0xD800 && cp <= 0xDBFF && end_ - p_ >= 6 && p_[0] == '\\' &&
                p_[1] == 'u') {
              const char* save = p_;
              p_ += 2;
              uint32_t lo = 0;
              if (!parseHex4(&lo, err)) return false;
              if (lo >= 0xDC00 && lo <= 0xDFFF) {
                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
              } else {
                p_ = save;
              }
            }
            appendUtf8(cp, out);
            break;
          }
          default:
            return fail("unknown escape", err);
        }
        continue;
      }
      out->push_back(c);
      ++p_;
    }
    return fail("unterminated string", err);
  }

  bool parseHex4(uint32_t* out, std::string* err) {
    if (end_ - p_ < 4) return fail("bad \\u escape", err);
    uint32_t v = 0;
    for (int i = 0; i < 4; ++i) {
      char c = p_[i];
      v <<= 4;
      if (c >= '0' && c <= '9') v |= static_cast<uint32_t>(c - '0');
      else if (c >= 'a' && c <= 'f') v |= static_cast<uint32_t>(c - 'a' + 10);
      else if (c >= 'A' && c <= 'F') v |= static_cast<uint32_t>(c - 'A' + 10);
      else return fail("bad hex digit", err);
    }
    p_ += 4;
    *out = v;
    return true;
  }

  static void appendUtf8(uint32_t cp, std::string* out) {
    if (cp < 0x80) {
      out->push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
      out->push_back(static_cast<char>(0xC0 | (cp >> 6)));
      out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
      out->push_back(static_cast<char>(0xE0 | (cp >> 12)));
      out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
      out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
      out->push_back(static_cast<char>(0xF0 | (cp >> 18)));
      out->push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
      out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
      out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
  }

  bool parseNumber(JsonValue* out, std::string* err) {
    const char* start = p_;
    bool isDouble = false;
    if (p_ < end_ && (*p_ == '-' || *p_ == '+')) ++p_;
    while (p_ < end_) {
      char c = *p_;
      if (c >= '0' && c <= '9') { ++p_; continue; }
      if (c == '.' || c == 'e' || c == 'E') { isDouble = true; ++p_; continue; }
      if ((c == '+' || c == '-') && (p_[-1] == 'e' || p_[-1] == 'E')) { ++p_; continue; }
      break;
    }
    if (p_ == start) return fail("invalid number", err);
    std::string text(start, static_cast<size_t>(p_ - start));
    if (!isDouble) {
      long long v = 0;
      if (sscanf(text.c_str(), "%lld", &v) == 1) {
        *out = JsonValue::integer(static_cast<int64_t>(v));
        return true;
      }
    }
    double d = 0.0;
    if (sscanf(text.c_str(), "%lf", &d) != 1) return fail("invalid number", err);
    *out = JsonValue::number(d);
    return true;
  }

  const char* p_;
  const char* end_;
};

void escapeString(const std::string& s, std::string* out) {
  out->push_back('"');
  for (unsigned char c : s) {
    switch (c) {
      case '"': *out += "\\\""; break;
      case '\\': *out += "\\\\"; break;
      case '\b': *out += "\\b"; break;
      case '\f': *out += "\\f"; break;
      case '\n': *out += "\\n"; break;
      case '\r': *out += "\\r"; break;
      case '\t': *out += "\\t"; break;
      default:
        if (c < 0x20) {
          char buf[8];
          snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned>(c));
          *out += buf;
        } else {
          out->push_back(static_cast<char>(c));
        }
    }
  }
  out->push_back('"');
}

std::string formatDouble(double d) {
  if (std::isnan(d) || std::isinf(d)) return "0";
  char buf[40];
  snprintf(buf, sizeof(buf), "%.10g", d);
  std::string s(buf);
  // 保证是合法 JSON 数字（整数值补 .0，避免 UI 侧类型抖动）
  if (s.find('.') == std::string::npos && s.find('e') == std::string::npos &&
      s.find('E') == std::string::npos && s.find("inf") == std::string::npos &&
      s.find("nan") == std::string::npos) {
    return s;  // %.10g 对 1e20 以上会自带 e，整数值保持整数形态即可
  }
  return s;
}

}  // namespace

const JsonValue& JsonValue::nullRef() {
  static const JsonValue kNullValue;
  return kNullValue;
}

JsonValue::JsonValue() : type_(Type::kNull), bool_(false), int_(0), double_(0.0) {}

JsonValue JsonValue::nullValue() { return JsonValue(); }
JsonValue JsonValue::boolean(bool v) {
  JsonValue j;
  j.type_ = Type::kBool;
  j.bool_ = v;
  return j;
}
JsonValue JsonValue::integer(int64_t v) {
  JsonValue j;
  j.type_ = Type::kInt;
  j.int_ = v;
  return j;
}
JsonValue JsonValue::number(double v) {
  JsonValue j;
  j.type_ = Type::kDouble;
  j.double_ = v;
  return j;
}
JsonValue JsonValue::str(const std::string& v) {
  JsonValue j;
  j.type_ = Type::kString;
  j.string_ = v;
  return j;
}
JsonValue JsonValue::str(const char* v) { return str(std::string(v ? v : "")); }
JsonValue JsonValue::arrayValue() {
  JsonValue j;
  j.type_ = Type::kArray;
  return j;
}
JsonValue JsonValue::objectValue() {
  JsonValue j;
  j.type_ = Type::kObject;
  return j;
}

bool JsonValue::asBool(bool def) const {
  if (type_ == Type::kBool) return bool_;
  if (type_ == Type::kInt) return int_ != 0;
  if (type_ == Type::kDouble) return double_ != 0.0;
  if (type_ == Type::kString) return !string_.empty() && string_ != "false";
  return def;
}

int64_t JsonValue::asInt(int64_t def) const {
  if (type_ == Type::kInt) return int_;
  if (type_ == Type::kDouble) return static_cast<int64_t>(double_);
  if (type_ == Type::kBool) return bool_ ? 1 : 0;
  if (type_ == Type::kString) {
    long long v = 0;
    if (sscanf(string_.c_str(), "%lld", &v) == 1) return static_cast<int64_t>(v);
  }
  return def;
}

double JsonValue::asDouble(double def) const {
  if (type_ == Type::kDouble) return double_;
  if (type_ == Type::kInt) return static_cast<double>(int_);
  if (type_ == Type::kBool) return bool_ ? 1.0 : 0.0;
  if (type_ == Type::kString) {
    double v = 0.0;
    if (sscanf(string_.c_str(), "%lf", &v) == 1) return v;
  }
  return def;
}

std::string JsonValue::asString(const std::string& def) const {
  if (type_ == Type::kString) return string_;
  if (type_ == Type::kInt) return std::to_string(int_);
  if (type_ == Type::kDouble) return formatDouble(double_);
  if (type_ == Type::kBool) return bool_ ? "true" : "false";
  return def;
}

const std::string& JsonValue::stringRef() const {
  static const std::string kEmpty;
  return type_ == Type::kString ? string_ : kEmpty;
}

size_t JsonValue::size() const {
  if (type_ == Type::kArray) return array_.size();
  if (type_ == Type::kObject) return object_.size();
  return 0;
}

bool JsonValue::contains(const std::string& key) const {
  if (type_ != Type::kObject) return false;
  for (const auto& kv : object_) {
    if (kv.first == key) return true;
  }
  return false;
}

const JsonValue& JsonValue::get(const std::string& key) const {
  if (type_ == Type::kObject) {
    for (const auto& kv : object_) {
      if (kv.first == key) return kv.second;
    }
  }
  return nullRef();
}

void JsonValue::setItem(const std::string& key, const JsonValue& v) {
  if (type_ != Type::kObject) {
    type_ = Type::kObject;
    object_.clear();
  }
  for (auto& kv : object_) {
    if (kv.first == key) { kv.second = v; return; }
  }
  object_.emplace_back(key, v);
}

void JsonValue::setItem(const std::string& key, const std::string& v) { setItem(key, str(v)); }
void JsonValue::setItem(const std::string& key, const char* v) { setItem(key, str(v)); }
void JsonValue::setItem(const std::string& key, int64_t v) { setItem(key, integer(v)); }
void JsonValue::setItem(const std::string& key, int v) { setItem(key, integer(v)); }
void JsonValue::setItem(const std::string& key, double v) { setItem(key, number(v)); }
void JsonValue::setItem(const std::string& key, bool v) { setItem(key, boolean(v)); }

bool JsonValue::eraseKey(const std::string& key) {
  if (type_ != Type::kObject) return false;
  for (auto it = object_.begin(); it != object_.end(); ++it) {
    if (it->first == key) { object_.erase(it); return true; }
  }
  return false;
}

const JsonValue& JsonValue::at(size_t index) const {
  if (type_ == Type::kArray && index < array_.size()) return array_[index];
  return nullRef();
}

void JsonValue::addItem(const JsonValue& v) {
  if (type_ != Type::kArray) {
    type_ = Type::kArray;
    array_.clear();
  }
  array_.push_back(v);
}
void JsonValue::addItem(const std::string& v) { addItem(str(v)); }
void JsonValue::addItem(const char* v) { addItem(str(v)); }
void JsonValue::addItem(int64_t v) { addItem(integer(v)); }
void JsonValue::addItem(int v) { addItem(integer(v)); }
void JsonValue::addItem(double v) { addItem(number(v)); }
void JsonValue::addItem(bool v) { addItem(boolean(v)); }

bool JsonValue::parse(const std::string& text, JsonValue* out, std::string* err) {
  if (text.empty()) {
    if (err) *err = "empty json text";
    return false;
  }
  Parser parser(text.data(), text.data() + text.size());
  JsonValue v;
  if (!parser.parseValue(&v, err)) return false;
  *out = v;
  return true;
}

void JsonValue::serializeTo(std::string* out) const {
  switch (type_) {
    case Type::kNull: *out += "null"; break;
    case Type::kBool: *out += bool_ ? "true" : "false"; break;
    case Type::kInt: *out += std::to_string(int_); break;
    case Type::kDouble: *out += formatDouble(double_); break;
    case Type::kString: escapeString(string_, out); break;
    case Type::kArray: {
      out->push_back('[');
      for (size_t i = 0; i < array_.size(); ++i) {
        if (i) out->push_back(',');
        array_[i].serializeTo(out);
      }
      out->push_back(']');
      break;
    }
    case Type::kObject: {
      out->push_back('{');
      for (size_t i = 0; i < object_.size(); ++i) {
        if (i) out->push_back(',');
        escapeString(object_[i].first, out);
        out->push_back(':');
        object_[i].second.serializeTo(out);
      }
      out->push_back('}');
      break;
    }
  }
}

std::string JsonValue::serialize() const {
  std::string out;
  out.reserve(256);
  serializeTo(&out);
  return out;
}

void JsonValue::mergePatch(const JsonValue& patch) {
  if (!patch.isObject()) {
    *this = patch;
    return;
  }
  if (!isObject()) {
    type_ = Type::kObject;
    object_.clear();
  }
  for (const auto& kv : patch.objectItems()) {
    const JsonValue& v = kv.second;
    if (v.isObject()) {
      // 目标也是对象 → 递归合并；否则整体替换
      bool found = false;
      for (auto& target : object_) {
        if (target.first == kv.first) {
          if (target.second.isObject()) target.second.mergePatch(v);
          else target.second = v;
          found = true;
          break;
        }
      }
      if (!found) object_.emplace_back(kv.first, v);
    } else {
      setItem(kv.first, v);
    }
  }
}

}  // namespace json
}  // namespace lumina
