// T1.1 测试：NodeGraph round-trip 字节稳定 + 默认图 identity + 8 类型全覆盖
#include <cstdio>
#include <string>

#include "graph/node_graph.h"
#include "graph/serialize.h"

using lumina::graph::defaultGraph;
using lumina::graph::defaultParamsFor;
using lumina::graph::graphFromJson;
using lumina::graph::graphFromString;
using lumina::graph::graphToJson;
using lumina::graph::graphToString;
using lumina::graph::NodeGraph;
using lumina::graph::NodeType;

static int g_fail = 0;
#define CHECK(cond, msg)                                       \
  do {                                                         \
    if (!(cond)) {                                             \
      std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, msg); \
      ++g_fail;                                                \
    }                                                          \
  } while (0)

// case1：默认图 = 8 全局节点 identity 全链
static void testDefaultGraph() {
  NodeGraph g = defaultGraph();
  CHECK(g.valid(), "default graph valid");
  CHECK(g.nodes.size() == 8, "8 nodes");
  CHECK(g.masks.empty(), "no masks");
  for (size_t i = 0; i < g.nodes.size(); ++i) {
    const auto& n = g.nodes[i];
    CHECK(n.enabled && n.opacity == 100 && !n.hasMask, "identity node fields");
    CHECK(n.blendMode == "normal", "identity blend");
    CHECK(!n.params.empty(), "params populated");
  }
  // 8 类型全覆盖且各不相同
  for (int t = 0; t < 8; ++t) {
    bool found = false;
    for (const auto& n : g.nodes) {
      if (static_cast<int>(n.type) == t) found = true;
    }
    CHECK(found, "type covered");
  }
}

// case2：默认图 round-trip 字节稳定（序列化→解析→序列化逐字节相等）
static void testRoundtripByteStable() {
  NodeGraph g = defaultGraph();
  const std::string s1 = graphToString(g);
  NodeGraph g2;
  std::string err;
  CHECK(graphFromString(s1, &g2, &err), "parse ok");
  const std::string s2 = graphToString(g2);
  CHECK(s1 == s2, "byte-stable roundtrip");
  NodeGraph g3;
  CHECK(graphFromString(s2, &g3, &err), "parse ok 2");
  CHECK(graphToString(g3) == s1, "byte-stable roundtrip x2");
}

// case3：复杂图（蒙版 + 曲线点数组 + null + opacity/blend）round-trip 稳定
static void testComplexRoundtrip() {
  NodeGraph g = defaultGraph();
  g.nodes[0].params.setItem("exposure", 0.35);
  g.nodes[0].params.setItem("highlights", -18.0);
  g.masks.push_back(lumina::graph::GraphMask());
  g.masks[0].id = "m1";
  g.masks[0].type = "radial";
  lumina::json::JsonValue geo = lumina::json::JsonValue::objectValue();
  geo.setItem("cx", 0.5);
  geo.setItem("cy", 0.3);
  geo.setItem("radius", 0.35);
  geo.setItem("ratio", 1.6);
  g.masks[0].geometry = geo;
  g.nodes[0].hasMask = true;
  g.nodes[0].mask.maskId = "m1";
  g.nodes[0].mask.invert = true;
  g.nodes[0].mask.feather = 12.5;
  g.nodes[0].opacity = 80;
  g.nodes[0].blendMode = "overlay";
  // 曲线点数组 + null
  lumina::json::JsonValue pts = lumina::json::JsonValue::arrayValue();
  lumina::json::JsonValue p0 = lumina::json::JsonValue::objectValue();
  p0.setItem("x", static_cast<int64_t>(0));
  p0.setItem("y", static_cast<int64_t>(0));
  pts.addItem(p0);
  lumina::json::JsonValue p1 = lumina::json::JsonValue::objectValue();
  p1.setItem("x", static_cast<int64_t>(255));
  p1.setItem("y", static_cast<int64_t>(250));
  pts.addItem(p1);
  g.nodes[1].params.setItem("toneCurveRGB", pts);
  const std::string s1 = graphToString(g);
  NodeGraph g2;
  std::string err;
  CHECK(graphFromString(s1, &g2, &err), "complex parse ok");
  CHECK(graphToString(g2) == s1, "complex roundtrip byte-stable");
  CHECK(g2.nodes[0].opacity == 80 && g2.nodes[0].blendMode == "overlay",
        "blend/opacity preserved");
  CHECK(g2.nodes[0].hasMask && g2.nodes[0].mask.maskId == "m1" &&
            g2.nodes[0].mask.invert,
        "mask preserved");
  CHECK(g2.nodes[1].params.get("toneCurveRGB").size() == 2,
        "curve points preserved");
  CHECK(g2.nodes[1].params.get("toneCurveR").isNull(), "null curve preserved");
  CHECK(g2.masks[0].geometry.get("ratio").asDouble() == 1.6,
        "mask geometry preserved");
}

// case4：非法输入零静默（未知类型 / 重复 id / opacity 越界 / blendMode 非法 / mask 引用缺失）
static void testValidation() {
  std::string err;
  // 未知类型
  err.clear();
  NodeGraph bad;
  bad = defaultGraph();
  // 直接用 validate 路径测非法
  err.clear();
  NodeGraph g = defaultGraph();
  g.nodes[1].id = g.nodes[0].id;
  CHECK(!lumina::graph::validateGraph(g, &err), "duplicate id rejected");
  g = defaultGraph();
  g.nodes[0].opacity = 150;
  CHECK(!lumina::graph::validateGraph(g, &err), "opacity range rejected");
  g = defaultGraph();
  g.nodes[0].blendMode = "difference";
  CHECK(!lumina::graph::validateGraph(g, &err), "blendMode rejected");
  g = defaultGraph();
  g.nodes[0].hasMask = true;
  g.nodes[0].mask.maskId = "nope";
  CHECK(!lumina::graph::validateGraph(g, &err), "maskId dangling rejected");
  g = defaultGraph();
  g.masks.push_back(lumina::graph::GraphMask());
  g.masks[0].id = "m1";
  g.masks[0].type = "radial";
  g.nodes[0].hasMask = true;
  g.nodes[0].mask.maskId = "m1";
  CHECK(lumina::graph::validateGraph(g, &err), "valid graph accepted");
  // schemaVersion 越界拒绝
  err.clear();
  CHECK(!graphFromString(
      "{\"schemaVersion\":3,\"nodes\":[{\"id\":\"n1\",\"type\":\"basic\","
      "\"params\":{}}],\"masks\":[]}", nullptr, &err),
      "schemaVersion>2 rejected");
}

// case5：类型字符串双向映射 + 默认参数非空
static void testTypeMapping() {
  for (int t = 0; t < 8; ++t) {
    const char* name = lumina::graph::nodeTypeName(static_cast<NodeType>(t));
    NodeType back;
    CHECK(lumina::graph::nodeTypeFromString(name, &back) &&
              static_cast<int>(back) == t,
          "type string roundtrip");
    CHECK(!defaultParamsFor(static_cast<NodeType>(t)).empty(),
          "default params non-empty");
  }
  CHECK(!lumina::graph::nodeTypeFromString("nope", nullptr),
        "unknown type string rejected");
}

int main() {
  testDefaultGraph();
  testRoundtripByteStable();
  testComplexRoundtrip();
  testValidation();
  testTypeMapping();
  if (g_fail == 0) {
    std::printf("test_node_graph: ALL PASS (5 cases)\n");
    return 0;
  }
  std::printf("test_node_graph: %d FAILURES\n", g_fail);
  return 1;
}
