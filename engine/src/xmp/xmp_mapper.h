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

// 曲线文本 ↔ {x,y}[]（导出供测试）。Lightroom 风格文本："0, 0 255, 255"，
// 兼容紧凑 "0,0" 与换行分隔。解析不出 ≥2 点时回退恒等曲线 (0,0)-(255,255)，
// 绝不返回空数组/全零曲线（下游会把空数组当全黑）。
std::string serializeCurve(const json::JsonValue& curve);
json::JsonValue parseCurve(const std::string& text);

}  // namespace xmp
}  // namespace lumina

#endif  // LUMINA_XMP_XMP_MAPPER_H_
