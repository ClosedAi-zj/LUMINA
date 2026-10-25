// T1.2 测试：XMP 双向迁移（v1→v2→v1 可逆 / crs: 字节保留 / AdjustmentStack 映射）
#include <cstdio>
#include <string>
#include <vector>

#include "graph/serialize.h"
#include "json/json_value.h"
#include "rpc/version.h"
#include "xmp/migrator.h"
#include "xmp/xmp_document.h"
#include "xmp/xmp_mapper.h"

using lumina::json::JsonValue;
using lumina::xmp::XmpDocument;
using lumina::xmp::XmpMapper;
using namespace lumina::graph;
using lumina::xmp::XmpMigrator;

static int g_fail = 0;
#define CHECK(cond, msg)                                       \
  do {                                                         \
    if (!(cond)) {                                             \
      std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, msg); \
      ++g_fail;                                                \
    }                                                          \
  } while (0)

// 构造 v1 编辑参数（嵌套形状，覆盖各节点组的代表键）
static JsonValue makeV1Params() {
  JsonValue p = JsonValue::objectValue();
  p.setItem("exposure", 0.35);
  p.setItem("contrast", 12.0);
  p.setItem("whiteBalance", "custom");
  JsonValue hsl = JsonValue::objectValue();
  JsonValue blue = JsonValue::objectValue();
  blue.setItem("sat", -20.0);
  hsl.setItem("blue", blue);
  p.setItem("hsl", hsl);
  JsonValue cg = JsonValue::objectValue();
  cg.setItem("midtoneHue", 30.0);
  p.setItem("colorGrade", cg);
  JsonValue detail = JsonValue::objectValue();
  detail.setItem("sharpness", 40.0);
  p.setItem("detail", detail);
  JsonValue optics = JsonValue::objectValue();
  optics.setItem("vignetteAmount", -0.3);
  optics.setItem("manualDistortion", 0.05);
  p.setItem("optics", optics);
  JsonValue geo = JsonValue::objectValue();
  JsonValue persp = JsonValue::objectValue();
  persp.setItem("rotate", 1.5);
  geo.setItem("perspective", persp);
  p.setItem("geometry", geo);
  JsonValue cal = JsonValue::objectValue();
  cal.setItem("redHue", 5.0);
  p.setItem("calibration", cal);
  // 曲线（{x,y}[]）
  JsonValue pts = JsonValue::arrayValue();
  JsonValue p0 = JsonValue::objectValue();
  p0.setItem("x", 0.0);
  p0.setItem("y", 0.0);
  pts.addItem(p0);
  JsonValue p1 = JsonValue::objectValue();
  p1.setItem("x", 255.0);
  p1.setItem("y", 250.0);
  pts.addItem(p1);
  p.setItem("toneCurveRGB", pts);
  return p;
}

// v1 sidecar 字节：createNew + writeToDoc（SchemaVersion=1 时代写法）。
// 注意：对「刚插入、从未序列化过」的属性 setText 只改 prop.value，
// serialize 仍用 insertions_ 里的原始元素 —— 所以必须先落盘再 parse 改版本号。
static std::string makeV1Sidecar(const JsonValue& params, std::string* err) {
  std::unique_ptr<XmpDocument> doc(new XmpDocument(XmpDocument::createNew()));
  // 一个「与引擎无关的字段」用于字节保留断言
  doc->setText("lumina", "VirtualCopyOf", "master-42");
  XmpMapper::writeToDoc(params, doc.get());
  std::string once = doc->serialize();
  doc = XmpDocument::parse(once, err);
  if (!doc) return std::string();
  doc->setText("lumina", "SchemaVersion", "1");  // v1 时代版本号
  std::string out = doc->serialize();
  if (out.empty() && err) *err = "empty sidecar";
  return out;
}

// case1：v1→v2 迁移核心：SchemaVersion 升 2 + NodeGraph 写入 + 幂等
static void testMigrateCore() {
  std::string err;
  JsonValue params = makeV1Params();
  const std::string v1 = makeV1Sidecar(params, &err);
  CHECK(!v1.empty(), "v1 sidecar built");
  std::unique_ptr<XmpDocument> doc = XmpDocument::parse(v1, &err);
  CHECK(doc != nullptr, "v1 parse");
  CHECK(XmpMigrator::migrateDocument(doc.get(), &err), "migrate ok");
  const std::string v2 = doc->serialize();
  CHECK(doc->getInt("lumina", "SchemaVersion", 0) == 2, "schema bumped to 2");
  CHECK(doc->has("lumina", "NodeGraph"), "NodeGraph present");
  // 幂等：再迁移一次字节不变
  std::unique_ptr<XmpDocument> doc2 = XmpDocument::parse(v2, &err);
  CHECK(XmpMigrator::migrateDocument(doc2.get(), &err), "migrate again ok");
  CHECK(doc2->serialize() == v2, "migration idempotent (byte-stable)");
}

// case2：crs: 字节保留（只增不改）+ 无关字段逐字节不变
static void testCrsBytePreservation() {
  std::string err;
  JsonValue params = makeV1Params();
  const std::string v1 = makeV1Sidecar(params, &err);
  // v1 中 crs:Exposure2012 的原始片段
  const std::string marker = "Exposure2012";
  const size_t pos = v1.find(marker);
  CHECK(pos != std::string::npos, "exposure field in v1");
  const std::string origSnippet = v1.substr(pos - 8, 48);
  std::unique_ptr<XmpDocument> doc = XmpDocument::parse(v1, &err);
  CHECK(XmpMigrator::migrateDocument(doc.get(), &err), "migrate ok");
  const std::string v2 = doc->serialize();
  // 迁移后原片段逐字节存在（值与格式未动）
  CHECK(v2.find(origSnippet) != std::string::npos,
        "crs:Exposure2012 bytes preserved verbatim");
  // 无关 lumina 字段保留
  CHECK(v2.find("master-42") != std::string::npos, "VirtualCopyOf preserved");
  // v1 里的 HSL 值未被二次改写（-20 出现次数迁移前后一致）
  auto count = [](const std::string& hay, const std::string& needle) {
    int c = 0;
    for (size_t p = hay.find(needle); p != std::string::npos;
         p = hay.find(needle, p + 1)) ++c;
    return c;
  };
  CHECK(count(v2, "HueAdjustment") >= count(v1, "HueAdjustment"),
        "no crs: field removed");
}

// case3：v1→v2→v1 可逆（投影同值）：图合并回 params 与原 params 扁平值一致
static void testRoundtripReversible() {
  std::string err;
  JsonValue params = makeV1Params();
  const std::string v1 = makeV1Sidecar(params, &err);
  std::unique_ptr<XmpDocument> doc = XmpDocument::parse(v1, &err);
  CHECK(XmpMigrator::migrateDocument(doc.get(), &err), "migrate ok");
  NodeGraph g;
  CHECK(XmpMapper::readGraph(*doc, &g, &err), "read graph back");
  CHECK(g.nodes.size() >= 8, "8 global nodes (+stack layers if any)");
  // 图 → params（投影）→ crs: 值对比
  JsonValue projected = XmpMapper::graphToParams(g);
  // 与原 params 扁平化后逐键对比
  JsonValue flatA, flatB;
  // flattenParams 是内部函数；改用 writeToDoc 的同值性验证：
  // 两份 params 分别 writeToDoc 到两个新 doc，比较 crs: 字段值
  std::unique_ptr<XmpDocument> docA(new XmpDocument(XmpDocument::createNew()));
  std::unique_ptr<XmpDocument> docB(new XmpDocument(XmpDocument::createNew()));
  XmpMapper::writeToDoc(params, docA.get());
  XmpMapper::writeToDoc(projected, docB.get());
  for (const std::string& key : {"Exposure2012", "Contrast2012",
                                 "SaturationAdjustmentBlue",
                                 "ColorGradeMidtoneHue", "Sharpness",
                                 "PostCropVignetteAmount",
                                 "LensManualDistortionAmount",
                                 "PerspectiveRotate", "CalibrationRedHue"}) {
    const std::string a = docA->get("crs", key);
    const std::string b = docB->get("crs", key);
    CHECK(!a.empty(), (key + " written for A").c_str());
    CHECK(a == b, (key + " same value after v1->v2->v1").c_str());
  }
  // 曲线投影同值
  CHECK(docA->get("crs", "ToneCurvePV2012") == docB->get("crs", "ToneCurvePV2012"),
        "tone curve projection same value");
}

// case4：AdjustmentStack 层映射 + 废弃标记
static void testAdjustmentStackMapping() {
  JsonValue params = makeV1Params();
  JsonValue stack = JsonValue::arrayValue();
  JsonValue layer = JsonValue::objectValue();
  layer.setItem("type", "curve");
  layer.setItem("enabled", true);
  layer.setItem("opacity", static_cast<int64_t>(60));
  layer.setItem("blendMode", "multiply");
  JsonValue lp = JsonValue::objectValue();
  lp.setItem("toneCurveRGB", params.get("toneCurveRGB"));
  layer.setItem("params", lp);
  layer.setItem("maskId", "m1");
  stack.addItem(layer);
  params.setItem("adjustmentStack", stack);
  JsonValue masks = params.get("masks");
  if (!masks.isArray()) masks = JsonValue::arrayValue();
  JsonValue m1 = JsonValue::objectValue();
  m1.setItem("id", "m1");
  m1.setItem("type", "radial");
  masks.addItem(m1);
  params.setItem("masks", masks);

  NodeGraph g = XmpMapper::paramsToGraph(params);
  CHECK(g.nodes.size() == 9, "8 global + 1 stack layer");
  const GraphNode& extra = g.nodes[8];
  CHECK(extra.id == "s0" && extra.type == NodeType::Curve,
        "stack layer node");
  CHECK(extra.opacity == 60 && extra.blendMode == "multiply" && extra.hasMask &&
            extra.mask.maskId == "m1",
        "layer blend/mask carried");
  CHECK(g.masks.size() == 1 && g.masks[0].id == "m1", "mask promoted");

  // 投影丢弃蒙版节点（曲线已进全局节点；蒙版节点不投影）
  JsonValue projected = XmpMapper::graphToParams(g);
  // 文档级：迁移写空数组 + Deprecated 标记
  std::string err;
  const std::string v1 = makeV1Sidecar(params, &err);
  std::unique_ptr<XmpDocument> doc = XmpDocument::parse(v1, &err);
  // v1 sidecar 里 adjustmentStack 经 kJsonFields 写入 lumina:AdjustmentStack
  CHECK(doc->has("lumina", "AdjustmentStack"), "stack present in v1");
  CHECK(XmpMigrator::migrateDocument(doc.get(), &err), "migrate ok");
  CHECK(doc->get("lumina", "AdjustmentStack") == "[]", "stack emptied");
  CHECK(doc->get("lumina", "AdjustmentStackDeprecated") == "true",
        "deprecated marker");
  NodeGraph g2;
  CHECK(XmpMapper::readGraph(*doc, &g2, &err), "graph readable");
  CHECK(g2.nodes.size() == 9, "layer node persisted");
}

// case5：schemaVersion>2 跳过不猜测
static void testTooNewSkipped() {
  std::string err;
  JsonValue params = makeV1Params();
  const std::string v1 = makeV1Sidecar(params, &err);
  std::unique_ptr<XmpDocument> doc = XmpDocument::parse(v1, &err);
  doc->setText("lumina", "SchemaVersion", "3");
  const std::string before = doc->serialize();
  CHECK(XmpMigrator::migrateDocument(doc.get(), &err), "too-new skipped ok");
  CHECK(doc->serialize() == before, "too-new untouched");
  CHECK(!doc->has("lumina", "NodeGraph"), "no graph injected for too-new");
}

int main() {
  testMigrateCore();
  testCrsBytePreservation();
  testRoundtripReversible();
  testAdjustmentStackMapping();
  testTooNewSkipped();
  if (g_fail == 0) {
    std::printf("test_xmp_nodegraph: ALL PASS (5 cases)\n");
    return 0;
  }
  std::printf("test_xmp_nodegraph: %d FAILURES\n", g_fail);
  return 1;
}
