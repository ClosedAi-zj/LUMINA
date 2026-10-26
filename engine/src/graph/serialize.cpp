#include "graph/serialize.h"

#include <set>

namespace lumina {
namespace graph {

namespace {

json::JsonValue maskToJson(const NodeMask& m) {
  json::JsonValue v = json::JsonValue::objectValue();
  v.setItem("maskId", m.maskId);
  v.setItem("invert", m.invert);
  v.setItem("feather", m.feather);
  return v;
}

bool maskFromJson(const json::JsonValue& v, NodeMask* m, std::string* err) {
  if (!v.isObject()) {
    if (err) *err = "mask must be object";
    return false;
  }
  m->maskId = v.get("maskId").asString();
  if (m->maskId.empty()) {
    if (err) *err = "mask.maskId empty";
    return false;
  }
  m->invert = v.get("invert").asBool(false);
  m->feather = v.get("feather").asDouble(0.0);
  return true;
}

json::JsonValue gmaskToJson(const GraphMask& m) {
  json::JsonValue v = json::JsonValue::objectValue();
  v.setItem("id", m.id);
  v.setItem("type", m.type);
  v.setItem("enabled", m.enabled);
  v.setItem("opacity", static_cast<int64_t>(m.opacity));
  // T2.5 v3：combine 恒写（迁移规则「v2→v3 只增 combine」由此满足）；
  // amount 缺省 100 时省略（写端省缺省值 → v2 存量 JSON 往返字节稳定）
  v.setItem("combine", maskCombineName(m.combine));
  if (m.amount != 100) v.setItem("amount", static_cast<int64_t>(m.amount));
  v.setItem("geometry", m.geometry);
  v.setItem("range", m.range);
  v.setItem("strokes", m.strokes);
  return v;
}

bool gmaskFromJson(const json::JsonValue& v, GraphMask* m, std::string* err) {
  if (!v.isObject()) {
    if (err) *err = "masks[] item must be object";
    return false;
  }
  m->id = v.get("id").asString();
  m->type = v.get("type").asString();
  if (m->id.empty() || m->type.empty()) {
    if (err) *err = "mask id/type required";
    return false;
  }
  m->enabled = v.get("enabled").asBool(true);
  m->opacity = static_cast<int>(v.get("opacity").asInt(100));
  // T2.5 v3：combine 缺省 replace（v2 蒙版无该字段 → 同值语义，不改观感）；
  // 出现但非法 → 显式失败（零静默，不猜测）
  if (v.contains("combine")) {
    if (!maskCombineFromString(v.get("combine").asString(), &m->combine)) {
      if (err) *err = "mask '" + m->id + "': invalid combine '" +
                      v.get("combine").asString() + "'";
      return false;
    }
  } else {
    m->combine = MaskCombine::Replace;
  }
  m->amount = static_cast<int>(v.get("amount").asInt(100));
  m->geometry = v.get("geometry");
  m->range = v.get("range");
  m->strokes = v.get("strokes");
  return true;
}

}  // namespace

json::JsonValue graphToJson(const NodeGraph& g) {
  json::JsonValue v = json::JsonValue::objectValue();
  v.setItem("schemaVersion", static_cast<int64_t>(g.schemaVersion));
  json::JsonValue nodes = json::JsonValue::arrayValue();
  for (const GraphNode& n : g.nodes) {
    json::JsonValue j = json::JsonValue::objectValue();
    j.setItem("id", n.id);
    j.setItem("type", nodeTypeName(n.type));
    j.setItem("order", static_cast<int64_t>(n.order));
    j.setItem("enabled", n.enabled);
    j.setItem("opacity", static_cast<int64_t>(n.opacity));
    j.setItem("blendMode", n.blendMode);
    j.setItem("mask", n.hasMask ? maskToJson(n.mask) : json::JsonValue::nullValue());
    j.setItem("params", n.params);
    nodes.addItem(j);
  }
  v.setItem("nodes", nodes);
  json::JsonValue masks = json::JsonValue::arrayValue();
  for (const GraphMask& m : g.masks) masks.addItem(gmaskToJson(m));
  v.setItem("masks", masks);
  return v;
}

bool graphFromJson(const json::JsonValue& v, NodeGraph* out, std::string* err) {
  if (!v.isObject() || out == nullptr) {
    if (err) *err = "graphFromJson: root must be object";
    return false;
  }
  const int64_t ver = v.get("schemaVersion").asInt(0);
  // T2.5：接受 ver ≤ 3（v2 读端向后可读；v3 = combine + 94 叶口径）
  if (ver < 1 || ver > 3) {
    if (err) *err = "graphFromJson: unsupported schemaVersion";
    return false;
  }
  out->schemaVersion = static_cast<int>(ver);
  out->nodes.clear();
  out->masks.clear();
  const json::JsonValue& nodes = v.get("nodes");
  if (!nodes.isArray() || nodes.empty()) {
    if (err) *err = "graphFromJson: nodes[] required";
    return false;
  }
  std::set<std::string> ids;
  for (size_t i = 0; i < nodes.size(); ++i) {
    const json::JsonValue& j = nodes.at(i);
    if (!j.isObject()) {
      if (err) *err = "nodes[" + std::to_string(i) + "] must be object";
      return false;
    }
    GraphNode n;
    n.id = j.get("id").asString();
    if (n.id.empty() || !ids.insert(n.id).second) {
      if (err) *err = "nodes[" + std::to_string(i) + "]: id empty/duplicate";
      return false;
    }
    if (!nodeTypeFromString(j.get("type").asString(), &n.type)) {
      if (err) *err = "nodes[" + n.id + "]: unknown type";
      return false;
    }
    n.order = static_cast<int>(j.get("order").asInt(static_cast<int64_t>(i)));
    n.enabled = j.get("enabled").asBool(true);
    n.opacity = static_cast<int>(j.get("opacity").asInt(100));
    if (n.opacity < 0 || n.opacity > 100) {
      if (err) *err = "nodes[" + n.id + "]: opacity out of range";
      return false;
    }
    n.blendMode = j.get("blendMode").asString("normal");
    if (!isBlendModeValid(n.blendMode)) {
      if (err) *err = "nodes[" + n.id + "]: invalid blendMode";
      return false;
    }
    const json::JsonValue& mask = j.get("mask");
    if (mask.isObject()) {
      if (!maskFromJson(mask, &n.mask, err)) return false;
      n.hasMask = true;
    }
    n.params = j.get("params").isObject() ? j.get("params")
                                          : json::JsonValue::objectValue();
    out->nodes.push_back(std::move(n));
  }
  const json::JsonValue& masks = v.get("masks");
  if (masks.isArray()) {
    for (size_t i = 0; i < masks.size(); ++i) {
      GraphMask m;
      if (!gmaskFromJson(masks.at(i), &m, err)) return false;
      out->masks.push_back(std::move(m));
    }
  }
  return true;
}

bool graphFromString(const std::string& text, NodeGraph* out, std::string* err) {
  json::JsonValue v;
  if (!json::JsonValue::parse(text, &v, err)) return false;
  return graphFromJson(v, out, err);
}

std::string graphToString(const NodeGraph& g) { return graphToJson(g).serialize(); }

bool validateGraph(const NodeGraph& g, std::string* err) {
  std::set<std::string> nodeIds;
  std::set<std::string> maskIds;
  for (const GraphMask& m : g.masks) {
    if (m.id.empty() || !maskIds.insert(m.id).second) {
      if (err) *err = "mask id empty/duplicate";
      return false;
    }
  }
  for (const GraphNode& n : g.nodes) {
    if (n.id.empty() || !nodeIds.insert(n.id).second) {
      if (err) *err = "node id empty/duplicate";
      return false;
    }
    if (!isBlendModeValid(n.blendMode) || n.opacity < 0 || n.opacity > 100) {
      if (err) *err = "node " + n.id + ": invalid blendMode/opacity";
      return false;
    }
    if (n.hasMask && maskIds.find(n.mask.maskId) == maskIds.end()) {
      if (err) *err = "node " + n.id + ": maskId not found";
      return false;
    }
  }
  return true;
}

}  // namespace graph
}  // namespace lumina
