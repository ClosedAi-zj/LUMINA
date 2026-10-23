// XMP sidecar 字节保留编辑器：解析记录节点原始字节区间，写回只替换被修改节点
#ifndef LUMINA_XMP_XMP_DOCUMENT_H_
#define LUMINA_XMP_XMP_DOCUMENT_H_

#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace lumina {
namespace xmp {

// 常用命名空间 URI 表（xmp.listNamespaces / 新属性声明用）
std::string namespaceUri(const std::string& prefix);

// 引擎认知的命名空间前缀（其余视为"未知命名空间"，原样缓存回写）
const std::vector<std::string>& knownPrefixes();

// XMP 属性：记录原始字节区间，序列化时只替换区间内容
struct Property {
  std::string prefix;
  std::string name;
  bool isAttribute = false;  // true: prefix:name="v"；false: <prefix:name>v</prefix:name>
  bool rawValue = false;     // true: value 是原始 XML 片段（不转义）
  bool selfClosing = false;  // 原节点是 <prefix:name/>，没有内容区间
  size_t start = 0;          // 元素/属性整体区间（删除时用）
  size_t end = 0;
  // 元素**内容**区间（不含开闭标签）。序列化替换内容时必须用它：
  // 若拿整元素区间去替换内部值，标签会被抹掉，sidecar 塌成一串裸文本，
  // Lightroom / Photoshop 将无法读取（曾导致第二次保存即摧毁 packet 结构）。
  size_t contentStart = 0;
  size_t contentEnd = 0;
  std::string value;
  bool touched = false;      // 本次会话是否被修改
  bool removed = false;      // 已删除（序列化时输出空串）
};

class XmpDocument {
 public:
  XmpDocument();

  // 解析；bytes 需含 <x:xmpmeta> 包，失败返回 nullptr 并填 err
  static std::unique_ptr<XmpDocument> parse(const std::string& bytes, std::string* err);
  // 构造全新空 sidecar（含 lumina: 命名空间）
  static XmpDocument createNew();

  bool valid() const { return valid_; }
  const std::string& raw() const { return raw_; }

  bool has(const std::string& prefix, const std::string& name) const;
  std::string get(const std::string& prefix, const std::string& name) const;
  bool getBool(const std::string& prefix, const std::string& name, bool def) const;
  int64_t getInt(const std::string& prefix, const std::string& name, int64_t def) const;
  double getDouble(const std::string& prefix, const std::string& name, double def) const;

  // 写文本值（自动 XML 转义）
  void setText(const std::string& prefix, const std::string& name, const std::string& value);
  // 写原始 XML 值（不转义，用于 rdf:Seq 等结构体）
  void setRaw(const std::string& prefix, const std::string& name, const std::string& rawXml);
  void remove(const std::string& prefix, const std::string& name);

  // 序列化：未触碰的节点逐字节保留；触碰的只替换其区间
  std::string serialize() const;

  // 已声明命名空间 → [(prefix, uri)]
  std::vector<std::pair<std::string, std::string>> declaredNamespaces() const;
  // 未知（非引擎认知）命名空间前缀
  std::vector<std::string> foreignPrefixes() const;
  // 当前所有属性 key（prefix:name）
  std::vector<std::string> propertyKeys() const;
  // 每个前缀的属性个数
  std::vector<std::pair<std::string, int>> namespaceFieldCounts() const;

 private:
  std::string key(const std::string& prefix, const std::string& name) const;
  void ensureNamespaceDeclared(const std::string& prefix);
  void insertProperty(const std::string& prefix, const std::string& name, bool rawValue,
                      const std::string& value);

  bool valid_ = false;
  std::string raw_;
  std::map<std::string, Property> props_;
  std::vector<std::pair<std::string, std::string>> namespaces_;  // 声明顺序
  // 新属性插入锚点（rdf:Description 内容起点）与开标签信息
  size_t descContentPos_ = 0;
  size_t descOpenTagEnd_ = 0;
  bool descSelfClosing_ = false;
  // 待插入的命名空间声明与元素（位置, 文本）
  std::vector<std::pair<size_t, std::string>> insertions_;
};

}  // namespace xmp
}  // namespace lumina

#endif  // LUMINA_XMP_XMP_DOCUMENT_H_
