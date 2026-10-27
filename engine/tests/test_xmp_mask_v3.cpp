// M2 T2.5 · 蒙版协议 v3 自测（N29：v2→v3 幂等字节稳定 / combine 缺省 replace / crs: 字节保留）
//
// 核心命题：**v3 协议升级对用户观感零改变**——v2 存量蒙版补 combine=replace 是同值
// 语义；crs: 字段逐字节保留；迁移幂等；v2 读端永远向后可读。
//
// 覆盖：
//   M1  GraphMask.combine 枚举映射 + 缺省 replace + 非法值显式失败（零静默）
//   M2  A6：序列化→反序列化→再序列化字节稳定（含 combine/amount/geometry/strokes）
//   M3  v2 图向后可读：schemaVersion=2 + 无 combine → 解析成功、语义为 replace
//   M4  文档级 v2→v3：只增 masks[*].combine + SchemaVersion=3；crs:/无关字段字节保留；
//       幂等（二次迁移字节不变）
//   M5  语义等价：v2 蒙版（无 combine）与补 combine:replace 的 v3 蒙版投影逐字相等
//   M6  叶 #94 键表：optics.chromaticAberration ↔ crs:ChromaticAberrationScale 往返
//   M7  存量迁移抽样方案（合成夹具 ×50）：v2 sidecar 批量迁移 → 全部升 3 + crs 保留 +
//       二次运行 0 变更（不动用户真实库）
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "core/config.h"
#include "core/str_util.h"
#include "core/thread_pool.h"
#include "db/database.h"
#include "db/photo_dao.h"
#include "graph/node_graph.h"
#include "graph/serialize.h"
#include "json/json_value.h"
#include "rpc/version.h"
#include "xmp/migrator.h"
#include "xmp/xmp_document.h"
#include "xmp/xmp_mapper.h"
#include "xmp/xmp_service.h"

using lumina::json::JsonValue;
using lumina::xmp::XmpDocument;
using lumina::xmp::XmpMapper;
using lumina::xmp::XmpMigrator;
using namespace lumina;
using namespace lumina::graph;

static int g_fail = 0;
// 注意：msg 必须先物化为 std::string 再经 c_str() 传 printf——直接把 std::string
// 临时对象塞进 printf 可变参数是 UB（MinGW GCC 8 不报错）：SSO 缓冲内容被当作
// char* 解引用 → 断言消息乱码 / 偶发段错误。
#define CHECK(cond, msg)                                                    \
  do {                                                                      \
    const std::string check_msg_ = (msg);                                   \
    if (cond) {                                                             \
      std::printf("  [PASS] %s\n", check_msg_.c_str());                     \
    } else {                                                                \
      std::printf("  [FAIL] %s:%d %s\n", __FILE__, __LINE__,                \
                  check_msg_.c_str());                                      \
      ++g_fail;                                                             \
    }                                                                       \
  } while (0)

namespace {

// 一个 v2 蒙版 JSON（无 combine / 无 amount —— 存量 3,639 张的形态）
JsonValue v2Mask(const std::string& id, const std::string& type, int opacity) {
  JsonValue m = JsonValue::objectValue();
  m.setItem("id", id);
  m.setItem("type", type);
  m.setItem("enabled", true);
  m.setItem("opacity", static_cast<int64_t>(opacity));
  JsonValue geo = JsonValue::objectValue();
  geo.setItem("cx", 0.5);
  geo.setItem("cy", 0.5);
  geo.setItem("rx", 0.3);
  m.setItem("geometry", geo);
  return m;
}

// 一张最小 v2 图 JSON（1 个全局节点 + 传入蒙版；键序与 v2 写端一致）
std::string v2GraphJson(const JsonValue& masks) {
  JsonValue node = JsonValue::objectValue();
  node.setItem("id", "n1");
  node.setItem("type", "basic");
  node.setItem("order", static_cast<int64_t>(0));
  node.setItem("enabled", true);
  node.setItem("opacity", static_cast<int64_t>(100));
  node.setItem("blendMode", "normal");
  node.setItem("mask", JsonValue::nullValue());
  JsonValue np = JsonValue::objectValue();
  np.setItem("exposure", 0.35);
  node.setItem("params", np);
  JsonValue nodes = JsonValue::arrayValue();
  nodes.addItem(node);
  JsonValue g = JsonValue::objectValue();
  g.setItem("schemaVersion", static_cast<int64_t>(2));
  g.setItem("nodes", nodes);
  g.setItem("masks", masks);
  return g.serialize();
}

// v2 sidecar 字节：createNew → 写 crs/lumina → serialize → parse → 改 SchemaVersion=2
// （XmpDocument 特性：新插入属性须先落一轮序列化才有稳定字节区间）
std::string makeV2Sidecar(const std::string& graphJson, const std::string& exposure,
                          std::string* err) {
  std::unique_ptr<XmpDocument> doc(new XmpDocument(XmpDocument::createNew()));
  doc->setText("crs", "Exposure2012", exposure);
  doc->setText("crs", "Contrast2012", "12");
  doc->setText("lumina", "VirtualCopyOf", "master-1");  // 无关字段（字节保留断言用）
  doc->setText("lumina", "NodeGraph", graphJson);
  std::string once = doc->serialize();
  doc = XmpDocument::parse(once, err);
  if (!doc) return std::string();
  doc->setText("lumina", "SchemaVersion", "2");
  std::string out = doc->serialize();
  if (out.empty() && err) *err = "empty sidecar";
  return out;
}

// crs: 字段原始片段截取（迁移前后逐字节比对）
std::string crsSnippet(const std::string& bytes) {
  const std::string marker = "Exposure2012";
  const size_t pos = bytes.find(marker);
  if (pos == std::string::npos || pos < 8) return std::string();
  return bytes.substr(pos - 8, 48);
}

}  // namespace

int main() {
  std::printf("=== xmp mask protocol v3 自测（T2.5）===\n");
  std::string err;

  // ---------------- M1 枚举映射 + 缺省 + 非法 ----------------
  std::printf("\n[M1] combine 枚举映射 / 缺省 replace / 非法值显式失败\n");
  {
    CHECK(std::string(maskCombineName(MaskCombine::Replace)) == "replace" &&
              std::string(maskCombineName(MaskCombine::Intersect)) == "intersect" &&
              std::string(maskCombineName(MaskCombine::Subtract)) == "subtract",
          "combine 字符串口径 replace/intersect/subtract");
    MaskCombine c = MaskCombine::Replace;
    CHECK(maskCombineFromString("intersect", &c) && c == MaskCombine::Intersect,
          "字符串 → 枚举往返");
    CHECK(!maskCombineFromString("inverted", &c), "非法 combine 串被拒绝");
  }

  // ---------------- M2 A6 字节稳定 ----------------
  std::printf("\n[M2] A6：序列化→反序列化→再序列化字节稳定\n");
  {
    NodeGraph g;
    g.schemaVersion = 3;
    JsonValue node = JsonValue::objectValue();
    node.setItem("id", "n1");
    node.setItem("type", "basic");
    node.setItem("order", static_cast<int64_t>(0));
    node.setItem("enabled", true);
    node.setItem("opacity", static_cast<int64_t>(100));
    node.setItem("blendMode", "normal");
    node.setItem("mask", JsonValue::nullValue());
    node.setItem("params", JsonValue::objectValue());
    g.nodes.push_back(GraphNode());
    g.nodes.back().id = "n1";
    g.nodes.back().type = NodeType::Basic;
    g.nodes.back().params.setItem("exposure", 0.35);

    GraphMask m1;
    m1.id = "m1";
    m1.type = "radial";
    m1.opacity = 80;
    m1.amount = 60;  // !=100 → 必须写出
    m1.combine = MaskCombine::Intersect;
    JsonValue geo = JsonValue::objectValue();
    geo.setItem("cx", 0.5);
    m1.geometry = geo;
    GraphMask m2;
    m2.id = "m2";
    m2.type = "brush";
    m2.combine = MaskCombine::Subtract;
    JsonValue strokes = JsonValue::arrayValue();
    strokes.addItem(JsonValue::str("stroke-1"));
    m2.strokes = strokes;
    GraphMask m3;  // 全缺省：combine=Replace、amount=100（写出 combine、省略 amount）
    m3.id = "m3";
    m3.type = "linear";
    g.masks = {m1, m2, m3};

    const std::string s1 = graphToString(g);
    NodeGraph back;
    CHECK(graphFromString(s1, &back, &err), "round1 parse");
    const std::string s2 = graphToString(back);
    CHECK(s1 == s2, "A6：serialize→parse→serialize 字节相等");
    NodeGraph back2;
    CHECK(graphFromString(s2, &back2, &err) && graphToString(back2) == s2,
          "A6：再一轮仍字节相等");
    CHECK(back.masks[0].combine == MaskCombine::Intersect &&
              back.masks[1].combine == MaskCombine::Subtract &&
              back.masks[2].combine == MaskCombine::Replace,
          "combine 往返保真");
    CHECK(back.masks[0].amount == 60 && back.masks[2].amount == 100,
          "amount 往返保真（缺省 100）");
    CHECK(s1.find("\"amount\":60") != std::string::npos &&
              s1.find("m3") != std::string::npos &&
              s1.substr(s1.find("\"id\":\"m3\"")).find("\"amount\"") == std::string::npos,
          "写端口径：amount!=100 写出、==100 省略");
    CHECK(back.schemaVersion == 3 && back.valid(), "v3 图 valid()=true");
  }

  // ---------------- M3 v2 图向后可读 ----------------
  std::printf("\n[M3] v2 图向后可读（无 combine → 语义 replace）\n");
  {
    JsonValue masks = JsonValue::arrayValue();
    masks.addItem(v2Mask("m1", "radial", 90));
    const std::string v2 = v2GraphJson(masks);
    NodeGraph g;
    CHECK(graphFromString(v2, &g, &err), "v2 图解析成功");
    CHECK(g.schemaVersion == 2 && g.valid(), "v2 schemaVersion 保留且 valid()=true");
    CHECK(g.masks.size() == 1 && g.masks[0].combine == MaskCombine::Replace,
          "v2 蒙版缺省 combine=replace（同值语义，不改观感）");
    // 非法 combine 显式失败（零静默）
    JsonValue bad = JsonValue::objectValue();
    bad.setItem("id", "mx");
    bad.setItem("type", "radial");
    bad.setItem("combine", "inverted");
    JsonValue badArr = JsonValue::arrayValue();
    badArr.addItem(bad);
    NodeGraph gb;
    std::string gbErr;
    CHECK(!graphFromString(v2GraphJson(badArr), &gb, &gbErr) && !gbErr.empty(),
          "非法 combine → 解析失败且给原因：" + gbErr);
  }

  // ---------------- M4 文档级 v2→v3 迁移 ----------------
  std::printf("\n[M4] v2→v3：只增 combine + SchemaVersion；crs: 字节保留；幂等\n");
  {
    JsonValue masks = JsonValue::arrayValue();
    masks.addItem(v2Mask("m1", "radial", 90));
    masks.addItem(v2Mask("m2", "linear", 100));
    const std::string v2Bytes = makeV2Sidecar(v2GraphJson(masks), "0.42", &err);
    CHECK(!v2Bytes.empty(), "v2 sidecar 构造");
    const std::string snippet = crsSnippet(v2Bytes);
    CHECK(!snippet.empty(), "crs:Exposure2012 片段截取");

    // 迁移前的 mask JSON（用于逐键比对）
    std::unique_ptr<XmpDocument> pre = XmpDocument::parse(v2Bytes, &err);
    NodeGraph gPre;
    CHECK(XmpMapper::readGraph(*pre, &gPre, &err), "迁移前图可读");
    CHECK(gPre.schemaVersion == 2, "迁移前图内版本=2");

    std::unique_ptr<XmpDocument> doc = XmpDocument::parse(v2Bytes, &err);
    CHECK(XmpMigrator::migrateDocument(doc.get(), &err), "migrateDocument ok");
    const std::string v3Bytes = doc->serialize();
    CHECK(doc->getInt("lumina", "SchemaVersion", 0) == 3, "SchemaVersion 2→3");
    CHECK(v3Bytes.find(snippet) != std::string::npos,
          "crs:Exposure2012 字节逐字保留（只增不改）");
    CHECK(v3Bytes.find("master-1") != std::string::npos, "无关 lumina 字段保留");
    CHECK(doc->get("lumina", "Masks").empty(), "lumina:Masks（v1 遗留）未被误写");

    // mask 逐键比对：旧键值不变 + 新增 combine=replace + 不新增 amount
    NodeGraph gPost;
    CHECK(XmpMapper::readGraph(*doc, &gPost, &err), "迁移后图可读");
    CHECK(gPost.schemaVersion == 3, "图内 schemaVersion 同步升 3");
    CHECK(gPost.masks.size() == 2, "蒙版数量不变");
    bool masksEqual = true;
    for (size_t i = 0; i < gPost.masks.size(); ++i) {
      const GraphMask& a = gPre.masks[i];
      const GraphMask& b = gPost.masks[i];
      if (a.id != b.id || a.type != b.type || a.enabled != b.enabled ||
          a.opacity != b.opacity ||
          a.geometry.serialize() != b.geometry.serialize() ||
          a.range.serialize() != b.range.serialize() ||
          a.strokes.serialize() != b.strokes.serialize()) {
        masksEqual = false;
      }
      if (b.combine != MaskCombine::Replace || b.amount != 100) masksEqual = false;
    }
    CHECK(masksEqual, "迁移语义：旧字段逐键不变 + combine=replace + 无 amount 新增");

    // 幂等：二次迁移字节不变
    std::unique_ptr<XmpDocument> doc2 = XmpDocument::parse(v3Bytes, &err);
    CHECK(XmpMigrator::migrateDocument(doc2.get(), &err), "二次迁移 ok");
    CHECK(doc2->serialize() == v3Bytes, "v2→v3→v3 幂等（字节稳定）");
  }

  // ---------------- M5 语义等价投影 ----------------
  std::printf("\n[M5] v2 蒙版与补 combine 的 v3 蒙版投影逐字相等\n");
  {
    JsonValue masks2 = JsonValue::arrayValue();
    masks2.addItem(v2Mask("m1", "radial", 90));
    NodeGraph g2;
    CHECK(graphFromString(v2GraphJson(masks2), &g2, &err), "v2 图解析");

    JsonValue v3Mask = v2Mask("m1", "radial", 90);
    v3Mask.setItem("combine", "replace");  // 手工补 v3 字段
    JsonValue masks3 = JsonValue::arrayValue();
    masks3.addItem(v3Mask);
    JsonValue g3v = JsonValue::objectValue();
    // 复用 v2 图骨架，仅替换 masks 与版本
    std::string tmp = v2GraphJson(masks3);
    JsonValue g3json;
    CHECK(JsonValue::parse(tmp, &g3json, &err), "v3 图 JSON 解析");
    g3json.setItem("schemaVersion", static_cast<int64_t>(3));
    NodeGraph g3;
    CHECK(graphFromString(g3json.serialize(), &g3, &err), "v3 图解析");

    JsonValue p2 = XmpMapper::graphToParams(g2);
    JsonValue p3 = XmpMapper::graphToParams(g3);
    const JsonValue& m2 = p2.get("masks").at(0);
    const JsonValue& m3 = p3.get("masks").at(0);
    CHECK(m2.serialize() == m3.serialize(),
          "投影逐字相等（" + m2.serialize() + "）");
    CHECK(m2.get("combine").asString() == "replace", "投影带 combine=replace");
    CHECK(!m2.contains("amount"), "amount==100 不写出（写端口径一致）");
  }

  // ---------------- M6 叶 #94 键表 ----------------
  std::printf("\n[M6] 叶 #94：optics.chromaticAberration ↔ crs:ChromaticAberrationScale\n");
  {
    JsonValue params = XmpMapper::defaultParams();
    JsonValue optics = JsonValue::objectValue();
    optics.setItem("chromaticAberration", 12.0);
    params.setItem("optics", optics);
    std::unique_ptr<XmpDocument> doc(new XmpDocument(XmpDocument::createNew()));
    XmpMapper::writeToDoc(params, doc.get());
    CHECK(doc->get("crs", "ChromaticAberrationScale") == "12",
          "写端：crs:ChromaticAberrationScale=12");
    JsonValue back;
    std::vector<std::string> warnings;
    XmpMapper::readFromDoc(*doc, &back, &warnings);
    CHECK(back.get("optics").get("chromaticAberration").asDouble() == 12.0,
          "读端：optics.chromaticAberration 往返");
    // 经图往返不丢（Lens 节点 3 叶）
    NodeGraph g = XmpMapper::paramsToGraph(back);
    // graphToJson(g) 按值返回临时 JsonValue；若在 range-for 里直接对
    // “临时.get(...).arrayItems()”迭代，__range 绑定的是临时对象内部成员的引用，
    // 临时对象在 range-init 全表达式末尾即销毁 → 迭代悬垂内存（flaky/段错误）。
    // 必须先落回具名左值延长生命周期。
    const JsonValue graphJson = graphToJson(g);
    bool found = false;
    for (const JsonValue& n : graphJson.get("nodes").arrayItems()) {
      if (n.get("type").asString() == "lens" &&
          n.get("params").get("chromaticAberration").asDouble() == 12.0) {
        found = true;
      }
    }
    CHECK(found, "paramsToGraph：CA 叶进 Lens 节点（不丢弃）");
    JsonValue projected = XmpMapper::graphToParams(g);
    CHECK(projected.get("optics").get("chromaticAberration").asDouble() == 12.0,
          "graphToParams：CA 叶投影回 optics");
  }

  // ---------------- M7 存量迁移抽样方案（合成夹具 ×50）----------------
  std::printf("\n[M7] 存量 3,639 张迁移抽样（合成 v2 sidecar ×50）\n");
  {
    const char* kHome = "D:/LUMINA/build/runtime/test-xmp-mask-v3/home";
    const char* kPhotoDir = "D:/LUMINA/build/runtime/test-xmp-mask-v3/photos";
    makeDirs(kPhotoDir);
    makeDirs(kHome);
    removeFile(std::string(kHome) + "/lumina.db");
    removeFile(std::string(kHome) + "/lumina.db-wal");
    removeFile(std::string(kHome) + "/lumina.db-shm");

    lumina::core::Config config;
    CHECK(config.init(kHome, &err), "config.init");
    config.load(&err);
    lumina::db::Database database;
    CHECK(database.open(config.dbPath(), &err) && database.runMigrations(&err),
          "database open+migrate");
    lumina::db::PhotoDao photos(&database);
    lumina::core::ThreadPool pool(2);
    lumina::xmp::XmpService xmp(&config, &photos, &database, &pool);
    XmpMigrator migrator(&config, &photos, &database, &xmp);

    const int64_t rootId = photos.ensureRoot(kPhotoDir, "test", &err);
    CHECK(rootId > 0, "root 就绪");

    const int kSamples = 50;
    std::vector<std::string> origBytes(kSamples);
    std::vector<std::string> sidecarPaths(kSamples);
    std::vector<int64_t> photoIds(kSamples, 0);
    int fixtureOk = 0;
    for (int i = 0; i < kSamples; ++i) {
      char name[32];
      snprintf(name, sizeof(name), "MV3_%04d.RW2", i);
      PhotoRecord p;
      p.fileName = name;
      p.filePath = std::string(kPhotoDir) + "/" + name;
      p.format = "RW2";
      p.rootId = rootId;
      p.folderId = photos.ensureFolder(rootId, "2026", "2026", &err);
      p.importedAt = lumina::nowMs();
      p.width = 64;
      p.height = 48;
      p.health = "ok";
      int64_t id = 0;
      if (!photos.upsert(p, &id, &err)) {
        std::printf("  [DBG] upsert fail i=%d err=%s\n", i, err.c_str());
        break;
      }
      photoIds[i] = id;
      // 蒙版数 1..3、第 2 张起带 amount!=100 的 v3 形态前置字段（模拟存量多样性）
      JsonValue masks = JsonValue::arrayValue();
      const int maskCount = 1 + (i % 3);
      for (int k = 0; k < maskCount; ++k) {
        JsonValue m = v2Mask("m" + std::to_string(k + 1),
                             k == 0 ? "radial" : (k == 1 ? "linear" : "brush"), 100 - k * 10);
        masks.addItem(m);
      }
      char exposure[16];
      snprintf(exposure, sizeof(exposure), "%.2f", 0.01 * i);
      std::string se;
      origBytes[i] = makeV2Sidecar(v2GraphJson(masks), exposure, &se);
      sidecarPaths[i] = lumina::replaceExtension(p.filePath, "xmp");
      if (origBytes[i].empty()) {
        std::printf("  [DBG] sidecar build fail i=%d err=%s\n", i, se.c_str());
      } else if (!lumina::writeFileAtomic(sidecarPaths[i], origBytes[i], &se)) {
        std::printf("  [DBG] write fail i=%d path=%s err=%s\n", i, sidecarPaths[i].c_str(),
                    se.c_str());
      } else if (!photos.setSidecar(id, sidecarPaths[i],
                                    lumina::fileMtimeMs(sidecarPaths[i]), true, &se)) {
        // 与真实导入路径一致：sidecar 落盘后在 photos 表登记（migrateAll 只扫已登记行）
        std::printf("  [DBG] setSidecar fail i=%d err=%s\n", i, se.c_str());
      } else {
        ++fixtureOk;
      }
    }
    CHECK(fixtureOk == kSamples,
          "50 张合成 v2 sidecar 就绪（实际 " + std::to_string(fixtureOk) + "；err=" + err + "）");

    int migratedCount = 0;
    CHECK(migrator.migrateAll(&migratedCount, &err, nullptr), "migrateAll 成功");
    CHECK(migratedCount >= kSamples, "migrateAll 扫过全部样本（count=" +
                                         std::to_string(migratedCount) + "）");

    int upgraded = 0;
    int crsKept = 0;
    int combineFilled = 0;
    for (int i = 0; i < kSamples; ++i) {
      std::string now;
      if (!lumina::readFile(sidecarPaths[i], &now, &err)) continue;
      std::unique_ptr<XmpDocument> doc = XmpDocument::parse(now, &err);
      if (!doc) continue;
      if (doc->getInt("lumina", "SchemaVersion", 0) == 3) ++upgraded;
      if (!crsSnippet(origBytes[i]).empty() &&
          now.find(crsSnippet(origBytes[i])) != std::string::npos) {
        ++crsKept;
      }
      NodeGraph g;
      if (XmpMapper::readGraph(*doc, &g, &err) && !g.masks.empty()) {
        bool allCombine = true;
        for (const GraphMask& m : g.masks) {
          if (m.combine != MaskCombine::Replace) allCombine = false;
        }
        if (allCombine) ++combineFilled;
      }
    }
    CHECK(upgraded == kSamples, "抽样断言①：50/50 升到 SchemaVersion=3");
    CHECK(crsKept == kSamples, "抽样断言②：50/50 crs: 字节保留");
    CHECK(combineFilled == kSamples, "抽样断言③：50/50 蒙版补 combine=replace");

    // 二次运行 0 变更（幂等：真实 migrateOne(photoId) 走一遍，字节必须不动）
    int stable = 0;
    for (int i = 0; i < kSamples; ++i) {
      std::string before;
      if (!lumina::readFile(sidecarPaths[i], &before, &err)) continue;
      std::string oneErr;
      if (!migrator.migrateOne(photoIds[i], &oneErr)) continue;
      std::string after;
      if (!lumina::readFile(sidecarPaths[i], &after, &err)) continue;
      if (before == after) ++stable;
    }
    CHECK(stable == kSamples, "抽样断言④：二次启动 50/50 字节不变（幂等）");
  }

  std::printf("\n=== 结果：%s（失败 %d 项）===\n", g_fail == 0 ? "全部通过" : "存在失败", g_fail);
  std::printf("[SKIP-COUNT] 0\n");
  return g_fail == 0 ? 0 : 1;
}
