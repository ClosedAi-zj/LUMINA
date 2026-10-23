// XMP 映射：EditParams(JSON) ↔ crs: / lumina: 字段（严格按 specs/XMP-字段映射表）
#ifndef LUMINA_XMP_XMP_MAPPER_H_
#define LUMINA_XMP_XMP_MAPPER_H_

#include <string>
#include <vector>

#include "json/json_value.h"
#include "xmp/xmp_document.h"

namespace lumina {
namespace xmp {

class XmpMapper {
 public:
  // 默认参数基线（processVersion 固定 15.4，全库统一）
  static json::JsonValue defaultParams();

  // XMP → params；warnings 收集非致命问题（未知 ProcessVersion 等）
  static void readFromDoc(const XmpDocument& doc, json::JsonValue* out,
                          std::vector<std::string>* warnings);

  // params → XMP；只写本次出现的字段，其余字节级保留
  static void writeToDoc(const json::JsonValue& params, XmpDocument* doc);

  // crs:ProcessVersion 是否高于引擎认知
  static bool processVersionTooNew(const XmpDocument& doc, std::string* found);
};

}  // namespace xmp
}  // namespace lumina

#endif  // LUMINA_XMP_XMP_MAPPER_H_
