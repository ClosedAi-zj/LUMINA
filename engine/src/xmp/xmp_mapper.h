// XMP 映射：EditParams(JSON) ↔ crs: / lumina: 字段（严格按 specs/XMP-字段映射表）
#ifndef LUMINA_XMP_XMP_MAPPER_H_
#define LUMINA_XMP_XMP_MAPPER_H_

#include <string>
#include <vector>

#include "graph/node_graph.h"
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

  // ---- T1.2：93 叶参数 ↔ NodeGraph（架构 §3.5）----

  // 现有 EditParams（嵌套形状）→ 图：8 全局节点各取本组出现过的值
  //（只搬运出现的键，不造默认值——保证投影同值、字节稳定）；
  // masks → 图级 masks[]；lumina:AdjustmentStack 层 → 追加带参节点（deprecated 语义）。
  static graph::NodeGraph paramsToGraph(const json::JsonValue& params);

  // 图 → params（crs: 投影口径）：仅合并无蒙版的全局节点参数回嵌套形状；
  // 蒙版节点无 crs: 等价字段，投影时丢弃；adjustmentStack 已废弃，不再回写。
  static json::JsonValue graphToParams(const graph::NodeGraph& g);

  // sidecar 读图：lumina:NodeGraph 文本 → 图（缺失/解析失败返回 false，零静默）
  static bool readGraph(const XmpDocument& doc, graph::NodeGraph* out,
                        std::string* err);
  // sidecar 写图：只增 lumina:NodeGraph + SchemaVersion=2（未触碰节点字节保留）
  static void writeGraph(const graph::NodeGraph& g, XmpDocument* doc);
};

// 曲线文本 ↔ {x,y}[]（导出供测试）。Lightroom 风格文本："0, 0 255, 255"，
// 兼容紧凑 "0,0" 与换行分隔。解析不出 ≥2 点时回退恒等曲线 (0,0)-(255,255)，
// 绝不返回空数组/全零曲线（下游会把空数组当全黑）。
std::string serializeCurve(const json::JsonValue& curve);
json::JsonValue parseCurve(const std::string& text);

}  // namespace xmp
}  // namespace lumina

#endif  // LUMINA_XMP_XMP_MAPPER_H_
