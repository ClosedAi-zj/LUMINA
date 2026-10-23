// XMP sidecar 字节保留编辑器实现
#include "xmp/xmp_document.h"

#include <algorithm>
#include <cstring>
#include <tuple>

#include "core/logger.h"
#include "core/str_util.h"

namespace lumina {
namespace xmp {
namespace {

struct ParsedTag {
  size_t start = 0;  // '<' 位置
  size_t end = 0;    // 紧跟 '>' 之后
  std::string name;
  std::string localName;
  std::string prefix;
  bool selfClosing = false;
  // (name, valueStart, valueEnd) —— valueStart/End 为原始字节区间（不含引号）
  std::vector<std::tuple<std::string, size_t, size_t>> attrSpans;
  std::vector<std::pair<std::string, std::string>> xmlnsDecls;
};

// 跳过注释 / CDATA / DOCTYPE，返回下一个扫描位置
size_t skipMarkup(const std::string& s, size_t i) {
  if (s.compare(i, 4, "<!--") == 0) {
    size_t e = s.find("-->", i);
    return e == std::string::npos ? s.size() : e + 3;
  }
  if (s.compare(i, 9, "<![CDATA[") == 0) {
    size_t e = s.find("]]>", i);
    return e == std::string::npos ? s.size() : e + 3;
  }
  size_t e = s.find('>', i);
  return e == std::string::npos ? s.size() : e + 1;
}

// 解析开标签；失败返回 false
bool parseTag(const std::string& s, size_t i, ParsedTag* tag) {
  size_t n = s.size();
  if (i >= n || s[i] != '<') return false;
  size_t p = i + 1;
  size_t nameStart = p;
  while (p < n && s[p] != ' ' && s[p] != '\t' && s[p] != '\n' && s[p] != '\r' && s[p] != '>' &&
         s[p] != '/' && s[p] != '<') {
    ++p;
  }
  if (p == nameStart) return false;
  tag->start = i;
  tag->name = s.substr(nameStart, p - nameStart);
  size_t colon = tag->name.find(':');
  if (colon == std::string::npos) {
    tag->prefix = "";
    tag->localName = tag->name;
  } else {
    tag->prefix = tag->name.substr(0, colon);
    tag->localName = tag->name.substr(colon + 1);
  }
  while (p < n) {
    while (p < n && (s[p] == ' ' || s[p] == '\t' || s[p] == '\n' || s[p] == '\r')) ++p;
    if (p >= n) return false;
    if (s[p] == '/') {
      size_t e = s.find('>', p);
      if (e == std::string::npos) return false;
      tag->selfClosing = true;
      tag->end = e + 1;
      return true;
    }
    if (s[p] == '>') {
      tag->end = p + 1;
      return true;
    }
    size_t attrNameStart = p;
    while (p < n && s[p] != '=' && s[p] != ' ' && s[p] != '\t' && s[p] != '\n' && s[p] != '\r' &&
           s[p] != '>' && s[p] != '/') {
      ++p;
    }
    if (p >= n) return false;
    if (s[p] != '=') continue;  // 无值属性，跳过
    ++p;
    if (p >= n) return false;
    char quote = s[p];
    if (quote != '"' && quote != '\'') return false;
    ++p;
    size_t valStart = p;
    while (p < n && s[p] != quote) ++p;
    if (p >= n) return false;
    size_t valEnd = p;
    ++p;
    std::string attrName = s.substr(attrNameStart, valStart - 2 - attrNameStart);
    std::string value = s.substr(valStart, valEnd - valStart);
    if (attrName.compare(0, 6, "xmlns:") == 0) {
      tag->xmlnsDecls.emplace_back(attrName.substr(6), xmlUnescape(value));
    } else if (attrName == "xmlns") {
      // 默认命名空间，忽略
    } else {
      tag->attrSpans.emplace_back(attrName, valStart, valEnd);
    }
  }
  return false;
}

std::string escapeAttr(const std::string& s) {
  std::string out;
  out.reserve(s.size() + 8);
  for (char c : s) {
    switch (c) {
      case '&': out += "&amp;"; break;
      case '<': out += "&lt;"; break;
      case '>': out += "&gt;"; break;
      case '"': out += "&quot;"; break;
      case '\'': out += "&apos;"; break;
      default: out.push_back(c);
    }
  }
  return out;
}

std::string escapeText(const std::string& s) {
  std::string out;
  out.reserve(s.size() + 8);
  for (char c : s) {
    switch (c) {
      case '&': out += "&amp;"; break;
      case '<': out += "&lt;"; break;
      case '>': out += "&gt;"; break;
      default: out.push_back(c);
    }
  }
  return out;
}

const std::vector<std::string>& knownList() {
  static const std::vector<std::string> kKnown = {
      "x",   "xmp",  "rdf",   "dc",    "photoshop", "crs",   "lrt",  "lumina",
      "exif", "tiff", "xmpMM", "xmpRights", "Iptc4xmpExt", "pdf", "aux",
      "photoshopExtended", "stEvt"};
  return kKnown;
}

}  // namespace

std::string namespaceUri(const std::string& prefix) {
  static const std::map<std::string, std::string> kUri = {
      {"x", "adobe:ns:meta/"},
      {"xmp", "http://ns.adobe.com/xap/1.0/"},
      {"rdf", "http://www.w3.org/1999/02/22-rdf-syntax-ns#"},
      {"dc", "http://purl.org/dc/elements/1.1/"},
      {"photoshop", "http://ns.adobe.com/photoshop/1.0/"},
      {"crs", "http://ns.adobe.com/camera-raw-settings/1.0/"},
      {"lrt", "http://lrtimelapse.com/ns/1.0/"},
      {"lumina", "http://ns.lumina.app/1.0/"},
      {"exif", "http://ns.adobe.com/exif/1.0/"},
      {"tiff", "http://ns.adobe.com/tiff/1.0/"},
      {"xmpMM", "http://ns.adobe.com/xap/1.0/mm/"},
      {"xmpRights", "http://ns.adobe.com/xap/1.0/rights/"},
      {"Iptc4xmpExt", "http://iptc.org/std/Iptc4xmpExt/2008-02-29/"},
      {"pdf", "http://ns.adobe.com/pdf/1.3/"},
      {"aux", "http://ns.adobe.com/exif/1.0/aux/"},
      {"stEvt", "http://ns.adobe.com/xap/1.0/sType/ResourceEvent#"}};
  auto it = kUri.find(prefix);
  return it == kUri.end() ? std::string() : it->second;
}

const std::vector<std::string>& knownPrefixes() { return knownList(); }

XmpDocument::XmpDocument() = default;

std::unique_ptr<XmpDocument> XmpDocument::parse(const std::string& bytes, std::string* err) {
  size_t metaStart = bytes.find("<x:xmpmeta");
  if (metaStart == std::string::npos) {
    if (err) *err = "no <x:xmpmeta> packet in sidecar";
    return nullptr;
  }
  size_t metaEnd = bytes.find("</x:xmpmeta>", metaStart);
  if (metaEnd == std::string::npos) {
    if (err) *err = "unterminated <x:xmpmeta> packet";
    return nullptr;
  }
  metaEnd += std::strlen("</x:xmpmeta>");

  auto doc = std::unique_ptr<XmpDocument>(new XmpDocument());
  doc->raw_ = bytes;
  doc->valid_ = true;

  std::vector<std::string> stack;
  bool sawDescription = false;
  size_t i = metaStart;
  while (i < metaEnd) {
    if (doc->raw_[i] != '<') {
      ++i;
      continue;
    }
    if (i + 1 < doc->raw_.size() && (doc->raw_[i + 1] == '?' || doc->raw_[i + 1] == '!')) {
      i = skipMarkup(doc->raw_, i);
      continue;
    }
    if (i + 1 < doc->raw_.size() && doc->raw_[i + 1] == '/') {
      size_t e = doc->raw_.find('>', i);
      if (e == std::string::npos || e >= metaEnd) break;
      std::string name = trim(doc->raw_.substr(i + 2, e - (i + 2)));
      if (!stack.empty() && stack.back() == name) stack.pop_back();
      i = e + 1;
      continue;
    }
    ParsedTag tag;
    if (!parseTag(doc->raw_, i, &tag)) {
      ++i;
      continue;
    }
    if (tag.end > metaEnd) break;
    for (const auto& decl : tag.xmlnsDecls) {
      bool exists = false;
      for (const auto& existing : doc->namespaces_) {
        if (existing.first == decl.first) {
          exists = true;
          break;
        }
      }
      if (!exists) doc->namespaces_.push_back(decl);
    }
    bool insideDescription = !stack.empty() && stack.back() == "rdf:Description";
    if (!insideDescription && tag.name == "rdf:Description" && !stack.empty() &&
        stack.back() == "rdf:RDF" && !sawDescription) {
      // 属性型属性：crs:Exposure2012="..." 挂在 rdf:Description 上
      for (const auto& span : tag.attrSpans) {
        const std::string& attrName = std::get<0>(span);
        size_t valStart = std::get<1>(span);
        size_t valEnd = std::get<2>(span);
        size_t colon = attrName.find(':');
        if (colon == std::string::npos) continue;
        std::string prefix = attrName.substr(0, colon);
        std::string local = attrName.substr(colon + 1);
        Property prop;
        prop.prefix = prefix;
        prop.name = local;
        prop.isAttribute = true;
        // 精确区间：属性名起点 = 值起点 - 属性名长度 - 2（'=' 与引号）
        size_t nameStartPos = valStart - attrName.size() - 2;
        prop.start = nameStartPos;
        prop.end = valEnd + 1;  // 含收尾引号
        prop.value = xmlUnescape(doc->raw_.substr(valStart, valEnd - valStart));
        doc->props_[doc->key(prefix, local)] = prop;
      }
      doc->descContentPos_ = tag.end;
      doc->descOpenTagEnd_ = tag.end;
      doc->descSelfClosing_ = tag.selfClosing;
      sawDescription = true;
    } else if (insideDescription) {
      if (tag.selfClosing) {
        Property prop;
        prop.prefix = tag.prefix;
        prop.name = tag.localName;
        prop.isAttribute = false;
        prop.start = tag.start;
        prop.end = tag.end;
        prop.selfClosing = true;
        prop.value = "";
        prop.rawValue = true;
        doc->props_[doc->key(tag.prefix, tag.localName)] = prop;
      } else {
        std::string closeTag = "</" + tag.name + ">";
        size_t closePos = doc->raw_.find(closeTag, tag.end);
        if (closePos != std::string::npos && closePos < metaEnd) {
          Property prop;
          prop.prefix = tag.prefix;
          prop.name = tag.localName;
          prop.isAttribute = false;
          prop.start = tag.start;
          prop.end = closePos + closeTag.size();
          // 单独记录内容区间：序列化时只替换这一段，开闭标签原样保留
          prop.contentStart = tag.end;
          prop.contentEnd = closePos;
          prop.value = doc->raw_.substr(tag.end, closePos - tag.end);
          prop.rawValue = true;
          doc->props_[doc->key(tag.prefix, tag.localName)] = prop;
        } else {
          logWarn("xmp", "unterminated element <" + tag.name + "> ignored");
        }
      }
    }
    if (!tag.selfClosing) stack.push_back(tag.name);
    i = tag.end;
  }
  if (!sawDescription) {
    if (err) *err = "no rdf:Description found in xmp packet";
    return nullptr;
  }
  if (doc->namespaces_.empty()) doc->namespaces_.emplace_back("rdf", namespaceUri("rdf"));
  return doc;
}

XmpDocument XmpDocument::createNew() {
  XmpDocument doc;
  doc.raw_ =
      "<?xpacket begin=\"\" id=\"W5M0MpCehiHzreSzNTczkc9d\"?>\n"
      "<x:xmpmeta xmlns:x=\"adobe:ns:meta/\">\n"
      " <rdf:RDF xmlns:rdf=\"http://www.w3.org/1999/02/22-rdf-syntax-ns#\">\n"
      "  <rdf:Description rdf:about=\"\" xmlns:crs=\"http://ns.adobe.com/camera-raw-settings/1.0/\" "
      "xmlns:lumina=\"http://ns.lumina.app/1.0/\">\n"
      "  </rdf:Description>\n"
      " </rdf:RDF>\n"
      "</x:xmpmeta>\n"
      "<?xpacket end=\"w\"?>\n";
  doc.valid_ = true;
  doc.namespaces_ = {{"x", namespaceUri("x")},
                     {"rdf", namespaceUri("rdf")},
                     {"crs", namespaceUri("crs")},
                     {"lumina", namespaceUri("lumina")}};
  size_t descStart = doc.raw_.find("<rdf:Description");
  size_t openEnd = doc.raw_.find('>', descStart);
  size_t content = doc.raw_.find("</rdf:Description>");
  doc.descOpenTagEnd_ = openEnd + 1;
  doc.descContentPos_ = content;
  doc.descSelfClosing_ = false;
  return doc;
}

std::string XmpDocument::key(const std::string& prefix, const std::string& name) const {
  return prefix + ":" + name;
}

bool XmpDocument::has(const std::string& prefix, const std::string& name) const {
  auto it = props_.find(key(prefix, name));
  return it != props_.end() && !it->second.removed;
}

std::string XmpDocument::get(const std::string& prefix, const std::string& name) const {
  auto it = props_.find(key(prefix, name));
  if (it == props_.end() || it->second.removed) return std::string();
  if (it->second.rawValue) return xmlUnescape(it->second.value);
  return it->second.value;
}

bool XmpDocument::getBool(const std::string& prefix, const std::string& name, bool def) const {
  std::string v = toLowerAscii(trim(get(prefix, name)));
  if (v == "true" || v == "1" || v == "yes") return true;
  if (v == "false" || v == "0" || v == "no") return false;
  return def;
}

int64_t XmpDocument::getInt(const std::string& prefix, const std::string& name, int64_t def) const {
  std::string v = trim(get(prefix, name));
  if (v.empty()) return def;
  long long parsed = 0;
  if (sscanf(v.c_str(), "%lld", &parsed) == 1) return static_cast<int64_t>(parsed);
  return def;
}

double XmpDocument::getDouble(const std::string& prefix, const std::string& name, double def) const {
  std::string v = trim(get(prefix, name));
  if (v.empty()) return def;
  double parsed = 0.0;
  if (sscanf(v.c_str(), "%lf", &parsed) == 1) return parsed;
  return def;
}

void XmpDocument::ensureNamespaceDeclared(const std::string& prefix) {
  for (const auto& ns : namespaces_) {
    if (ns.first == prefix) return;
  }
  std::string uri = namespaceUri(prefix);
  if (uri.empty()) {
    logWarn("xmp", "unknown namespace prefix '" + prefix + "'; no declaration added");
    return;
  }
  namespaces_.emplace_back(prefix, uri);
  if (descOpenTagEnd_ > 1) {
    std::string decl = " xmlns:" + prefix + "=\"" + escapeAttr(uri) + "\"";
    insertions_.emplace_back(descOpenTagEnd_ - 1, decl);
  }
}

void XmpDocument::insertProperty(const std::string& prefix, const std::string& name, bool rawValue,
                                 const std::string& value) {
  ensureNamespaceDeclared(prefix);
  std::string element = "<" + prefix + ":" + name + ">" +
                        (rawValue ? value : escapeText(value)) + "</" + prefix + ":" + name + ">";
  insertions_.emplace_back(descContentPos_, element);
}

void XmpDocument::setText(const std::string& prefix, const std::string& name,
                          const std::string& value) {
  std::string k = key(prefix, name);
  auto it = props_.find(k);
  if (it == props_.end() || it->second.removed) {
    Property prop;
    prop.prefix = prefix;
    prop.name = name;
    prop.isAttribute = false;
    prop.rawValue = false;
    prop.value = value;
    prop.start = 0;
    prop.end = 0;
    prop.touched = true;
    props_[k] = prop;
    insertProperty(prefix, name, false, value);
    return;
  }
  it->second.value = value;
  it->second.rawValue = false;
  it->second.touched = true;
}

void XmpDocument::setRaw(const std::string& prefix, const std::string& name,
                         const std::string& rawXml) {
  std::string k = key(prefix, name);
  auto it = props_.find(k);
  if (it == props_.end() || it->second.removed) {
    Property prop;
    prop.prefix = prefix;
    prop.name = name;
    prop.isAttribute = false;
    prop.rawValue = true;
    prop.value = rawXml;
    prop.start = 0;
    prop.end = 0;
    prop.touched = true;
    props_[k] = prop;
    insertProperty(prefix, name, true, rawXml);
    return;
  }
  it->second.value = rawXml;
  it->second.rawValue = true;
  it->second.touched = true;
}

void XmpDocument::remove(const std::string& prefix, const std::string& name) {
  auto it = props_.find(key(prefix, name));
  if (it == props_.end()) return;
  if (it->second.start == 0 && it->second.end == 0) {
    props_.erase(it);
    return;
  }
  it->second.touched = true;
  it->second.removed = true;
  it->second.value.clear();
  it->second.rawValue = false;
}

std::string XmpDocument::serialize() const {
  struct Edit {
    size_t start;
    size_t end;
    std::string replacement;
  };
  std::vector<Edit> edits;
  for (const auto& kv : props_) {
    const Property& p = kv.second;
    if (!p.touched) continue;
    if (p.start == 0 && p.end == 0) continue;  // 新建属性由 insertions_ 负责
    if (p.removed) {
      edits.push_back(Edit{p.start, p.end, std::string()});
      continue;
    }
    if (p.isAttribute) {
      std::string attr = p.prefix + ":" + p.name + "=\"" + escapeAttr(p.value) + "\"";
      edits.push_back(Edit{p.start, p.end, attr});
    } else if (p.selfClosing) {
      // 原来是 <ns:name/>：值为空就原样保留（不动），有值才展开成成对标签
      if (p.value.empty()) continue;
      std::string elem = "<" + p.prefix + ":" + p.name + ">" +
                         (p.rawValue ? p.value : escapeText(p.value)) + "</" + p.prefix + ":" +
                         p.name + ">";
      edits.push_back(Edit{p.start, p.end, elem});
    } else if (p.contentStart > 0 || p.contentEnd > 0) {
      // 已有成对标签：**只替换内容区间**，开闭标签必须原样保留。
      // 内容区间长度为 0（空元素 <ns:name></ns:name>）时等价于插入新值，标签同样保留。
      edits.push_back(Edit{p.contentStart, p.contentEnd,
                           p.rawValue ? p.value : escapeText(p.value)});
    } else {
      // 没有内容区间（例如空元素 <ns:name></ns:name> 已被上面覆盖，这里是兜底）
      edits.push_back(Edit{p.start, p.end, p.rawValue ? p.value : escapeText(p.value)});
    }
  }
  std::sort(edits.begin(), edits.end(),
            [](const Edit& a, const Edit& b) { return a.start > b.start; });
  std::string out = raw_;
  for (const Edit& e : edits) {
    if (e.start > out.size() || e.end > out.size() || e.start > e.end) continue;
    out.replace(e.start, e.end - e.start, e.replacement);
  }
  std::vector<std::pair<size_t, std::string>> ins = insertions_;
  std::sort(ins.begin(), ins.end(),
            [](const std::pair<size_t, std::string>& a, const std::pair<size_t, std::string>& b) {
              return a.first > b.first;
            });
  for (const auto& in : ins) {
    if (in.first > out.size()) continue;
    out.insert(in.first, in.second);
  }
  return out;
}

std::vector<std::pair<std::string, std::string>> XmpDocument::declaredNamespaces() const {
  return namespaces_;
}

std::vector<std::string> XmpDocument::foreignPrefixes() const {
  const std::vector<std::string>& known = knownList();
  std::vector<std::string> out;
  for (const auto& ns : namespaces_) {
    bool isKnown = false;
    for (const std::string& k : known) {
      if (k == ns.first) {
        isKnown = true;
        break;
      }
    }
    if (!isKnown) out.push_back(ns.first);
  }
  return out;
}

std::vector<std::string> XmpDocument::propertyKeys() const {
  std::vector<std::string> out;
  for (const auto& kv : props_) {
    if (!kv.second.removed) out.push_back(kv.first);
  }
  return out;
}

std::vector<std::pair<std::string, int>> XmpDocument::namespaceFieldCounts() const {
  std::vector<std::pair<std::string, int>> out;
  for (const auto& ns : namespaces_) {
    int count = 0;
    for (const auto& kv : props_) {
      if (kv.second.removed) continue;
      if (kv.first.rfind(ns.first + ":", 0) == 0) ++count;
    }
    out.emplace_back(ns.first, count);
  }
  return out;
}

}  // namespace xmp
}  // namespace lumina
