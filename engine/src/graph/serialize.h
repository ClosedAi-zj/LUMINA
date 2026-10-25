// T1.1：NodeGraph ↔ JSON 编解码（架构 §3.4 Schema）
// - 字段写入顺序固定 → 同图两次序列化逐字节相等（round-trip 字节稳定）
// - 解析做结构校验（type/blendMode/opacity/幂等 id），错误零静默
#ifndef LUMINA_GRAPH_SERIALIZE_H_
#define LUMINA_GRAPH_SERIALIZE_H_

#include <string>

#include "graph/node_graph.h"
#include "json/json_value.h"

namespace lumina {
namespace graph {

// 图 → JSON（字段顺序固定；schemaVersion=2）
json::JsonValue graphToJson(const NodeGraph& g);

// JSON → 图。err 写明首个错误。接受 §3.4 schema（mask 可为 null / 对象缺失）。
bool graphFromJson(const json::JsonValue& v, NodeGraph* out, std::string* err);

// 便捷：解析文本 → 图
bool graphFromString(const std::string& text, NodeGraph* out, std::string* err);

// 图 → 规范化 JSON 文本（serialize；解析再序列化应逐字节相等）
std::string graphToString(const NodeGraph& g);

// 校验（结构层面：id 唯一非空、type 合法、blendMode 合法、opacity 0..100、
// mask 引用的 maskId 存在于图级 masks）。通过返回 true。
bool validateGraph(const NodeGraph& g, std::string* err);

}  // namespace graph
}  // namespace lumina

#endif  // LUMINA_GRAPH_SERIALIZE_H_
