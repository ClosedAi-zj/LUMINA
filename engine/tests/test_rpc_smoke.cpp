// RPC 冒烟自测：装配最小引擎服务集，经 Dispatcher 实际分发关键方法
// 覆盖：握手 / 未知方法 / 查询 / 编辑会话（含 v1.0.1 edit.endSession 四字段）/
//       预设 / 帧编解码 / 自检 graceful failure
#include <cstdio>
#include <string>
#include <vector>

#include "core/config.h"
#include "core/job_manager.h"
#include "core/str_util.h"
#include "core/thread_pool.h"
#include "db/album_dao.h"
#include "db/database.h"
#include "db/fts_indexer.h"
#include "db/health_dao.h"
#include "db/photo_dao.h"
#include "edit/edit_session.h"
#include "edit/virtual_copy.h"
#include "health/backup.h"
#include "health/scanner.h"
#include "import/importer.h"
#include "json/json_value.h"
#include "library/query_engine.h"
#include "mock/generator.h"
#include "raw/raw_decoder.h"
#include "rpc/dispatcher.h"
#include "rpc/frame.h"
#include "rpc/methods.h"
#include "rpc/rpc_server.h"
#include "thumb/thumbnail_service.h"
#include "xmp/migrator.h"
#include "xmp/xmp_service.h"

using namespace lumina;

namespace {

int gFailures = 0;

void check(bool condition, const std::string& name) {
  if (condition) {
    printf("  [PASS] %s\n", name.c_str());
  } else {
    printf("  [FAIL] %s\n", name.c_str());
    ++gFailures;
  }
}

const char* kDir = "D:/LUMINA/build/runtime/test-rpc";
const char* kHome = "D:/LUMINA/build/runtime/test-rpc/home";

// photos.root_id / photos.folder_id 是外键（PRAGMA foreign_keys=ON），必须指向真实行，
// 否则 upsert 直接 FOREIGN KEY constraint failed（不是"插不进去就算了"的软失败）。
int64_t gRootId = 0;
int64_t gFolderId = 0;

// 插入一条照片记录（不生成影像文件，仅索引层）
int64_t insertPhoto(db::PhotoDao* photos, const std::string& fileName, const std::string& location,
                    const std::string& date, int rating, int64_t capturedAt) {
  PhotoRecord p;
  p.fileName = fileName;
  p.filePath = std::string(kDir) + "/" + fileName;
  p.format = "RW2";
  p.rootId = gRootId;
  p.folderId = gFolderId;
  p.capturedAt = capturedAt;
  p.importedAt = nowMs();
  p.camera = "DC-S5M2";
  p.location = location;  // FTS 列：中文地点名
  p.date = date;          // FTS 列：YYYY-MM
  p.rating = rating;
  p.width = 160;
  p.height = 112;
  p.health = "ok";
  p.sidecarPath = replaceExtension(p.filePath, "xmp");
  int64_t id = 0;
  std::string err;
  if (!photos->upsert(p, &id, &err)) {
    printf("  [FAIL] insertPhoto %s: %s\n", fileName.c_str(), err.c_str());
    ++gFailures;
    return 0;
  }
  return id;
}

}  // namespace

int main() {
  printf("=== RPC 冒烟自测 ===\n");
  makeDirs(kDir);
  // 清理上次残留，保证可重复执行（sidecar 也必须清：否则上一轮写的 P9000001.xmp 会让
  // 本轮 edit.beginSession 判定"外部修改"，用例 7/8/9 会连锁失败——这是测试卫生，不是引擎 bug）
  removeFile(std::string(kHome) + "/db/lumina.db");
  removeFile(std::string(kHome) + "/db/lumina.db-wal");
  removeFile(std::string(kHome) + "/db/lumina.db-shm");
  for (int i = 1; i <= 3; ++i) {
    char sidecar[256];
    snprintf(sidecar, sizeof(sidecar), "%s/P900000%d.xmp", kDir, i);
    removeFile(sidecar);
  }

  std::string err;
  core::Config config;
  if (!config.init(kHome, &err)) {
    printf("  [FAIL] config.init: %s\n", err.c_str());
    return 1;
  }
  config.load(&err);

  db::Database database;
  if (!database.open(config.dbPath(), &err) || !database.runMigrations(&err)) {
    printf("  [FAIL] database open: %s\n", err.c_str());
    return 1;
  }
  db::PhotoDao photos(&database);
  db::AlbumDao albums(&database);
  db::HealthDao health(&database);
  db::FtsIndexer fts(&database);
  core::ThreadPool pool(2);

  rpc::Dispatcher dispatcher;
  rpc::RpcServer server(&dispatcher);
  JobManager jobs(&pool, server.notifyFn());

  library::QueryEngine query(&config, &database, &photos);
  thumb::ThumbnailService thumbs(&config, &photos, &pool,
                                 [](int64_t, int, const std::string&) {});
  raw::RawDecoder decoder;
  xmp::XmpService xmp(&config, &photos, &database, &pool);
  xmp::XmpMigrator migrator(&config, &photos, &database, &xmp);
  edit::EditSessionManager sessions(&xmp, &photos);
  edit::VirtualCopyService vcopy(&config, &database, &photos, &xmp);
  // M2 T2.2b：四级表身份层（relocate RPC 入口走它）
  db::ImageDao imageDao(&database, &xmp);
  import::Importer importer(&config, &photos, &imageDao, &database, &fts, &decoder, &xmp,
                            [](const std::string&, const json::JsonValue&) {});
  health::HealthScanner healthScanner(&config, &photos, &health,
                                      [](const std::string&, const json::JsonValue&) {});
  health::BackupService backup(&config, &photos, &database,
                               [](const std::string&, const json::JsonValue&) {});
  mock::MockGenerator mock(&config, &photos, &database, &fts, &health, &decoder, &xmp);

  rpc::EngineServices s;
  s.config = &config;
  s.db = &database;
  s.photos = &photos;
  s.images = &imageDao;
  s.albums = &albums;
  s.health = &health;
  s.fts = &fts;
  s.query = &query;
  s.thumbs = &thumbs;
  s.decoder = &decoder;
  s.xmp = &xmp;
  s.migrator = &migrator;
  s.sessions = &sessions;
  s.vcopy = &vcopy;
  s.importer = &importer;
  s.healthScanner = &healthScanner;
  s.backup = &backup;
  s.watcher = nullptr;  // 冒烟测试不启动文件监测
  s.mock = &mock;
  s.jobs = &jobs;
  s.server = &server;

  rpc::registerMethods(&dispatcher, &s);
  rpc::seedPresetData(&s);
  check(dispatcher.hasMethod("system.initialize"), "setup: system.initialize 已注册");
  check(dispatcher.hasMethod("raw.selfTest"), "setup: raw.selfTest 已注册（v1.0.1）");
  check(dispatcher.hasMethod("edit.endSession"), "setup: edit.endSession 已注册");

  // ---------- 用例 1：握手 ----------
  {
    rpc::RpcResponse r = dispatcher.dispatch("system.initialize", json::JsonValue::objectValue());
    check(!r.isError, "case1: system.initialize 无错误");
    check(r.result.get("protocolVersion").asString() == "1.0", "case1: protocolVersion=1.0");
    check(r.result.get("engineVersion").asString() == "1.1.1", "case1: engineVersion=1.1.1");
    check(r.result.get("capabilities").get("libraw").asBool(false), "case1: capabilities.libraw");
  }

  // ---------- 用例 2：ping ----------
  {
    rpc::RpcResponse r = dispatcher.dispatch("system.ping", json::JsonValue::objectValue());
    check(!r.isError && r.result.get("pong").asBool(false), "case2: system.ping → pong");
  }

  // ---------- 用例 3：未知方法 → -32601 不崩溃 ----------
  {
    rpc::RpcResponse r = dispatcher.dispatch("no.such.method", json::JsonValue::objectValue());
    check(r.isError && r.code == rpc::kMethodNotFound, "case3: 未知方法 → -32601");
  }

  // ---------- 用例 4：空库查询 ----------
  {
    rpc::RpcResponse r = dispatcher.dispatch("library.queryPhotos", json::JsonValue::objectValue());
    check(!r.isError, "case4: library.queryPhotos 无错误");
    check(r.result.get("total").isInt() || r.result.get("total").isDouble(),
          "case4: total 为数值");
    check(r.result.get("items").isArray(), "case4: items 为数组");
  }

  // ---------- 用例 5：灌入数据后分页查询 ----------
  {
    gRootId = photos.ensureRoot(kDir, "test-rpc", &err);
    gFolderId = photos.ensureFolder(gRootId, "2026", "2026", &err);
    check(gRootId > 0 && gFolderId > 0, "case5: 建根目录/文件夹（外键前置）");
    insertPhoto(&photos, "P9000001.RW2", "四姑娘山", "2026-08", 4, nowMs() - 86400000LL * 3);
    insertPhoto(&photos, "P9000002.RW2", "松赞林寺", "2026-08", 2, nowMs() - 86400000LL * 2);
    insertPhoto(&photos, "P9000003.RW2", "四姑娘山", "2026-07", 0, nowMs() - 86400000LL);
    std::string ftsErr;
    fts.rebuild(&ftsErr);
    json::JsonValue params = json::JsonValue::objectValue();
    json::JsonValue page = json::JsonValue::objectValue();
    page.setItem("limit", 2);
    page.setItem("offset", 0);
    params.setItem("page", page);
    rpc::RpcResponse r = dispatcher.dispatch("library.queryPhotos", params);
    check(!r.isError, "case5: 分页查询无错误");
    if (r.isError) printf("  [INFO] case5 error %d: %s\n", r.code, r.message.c_str());
    const std::vector<json::JsonValue>& items5 = r.result.get("items").arrayItems();
    check(items5.size() == 2, "case5: limit=2 返回 2 条");
    check(r.result.get("total").asInt() == 3, "case5: total=3");
    check(!items5.empty() && items5[0].contains("thumbPath"), "case5: 汇总字段含 thumbPath");
    // 深分页（offset=2）不丢数据
    json::JsonValue deep = json::JsonValue::objectValue();
    json::JsonValue deepPage = json::JsonValue::objectValue();
    deepPage.setItem("limit", 2);
    deepPage.setItem("offset", 2);
    deep.setItem("page", deepPage);
    rpc::RpcResponse rd = dispatcher.dispatch("library.queryPhotos", deep);
    check(!rd.isError && rd.result.get("items").arrayItems().size() == 1, "case5: 深分页 offset=2");
  }

  // ---------- 用例 6：FTS5 中文搜索 ----------
  {
    json::JsonValue params = json::JsonValue::objectValue();
    params.setItem("text", "四姑娘山");
    rpc::RpcResponse r = dispatcher.dispatch("library.search", params);
    check(!r.isError, "case6: library.search 无错误");
    check(r.result.get("total").asInt() == 2, "case6: 搜索「四姑娘山」命中 2 条");
    const std::vector<json::JsonValue>& items = r.result.get("items").arrayItems();
    check(!items.empty() && items[0].get("fileName").asString() == "P9000001.RW2",
          "case6: 首条为 P9000001.RW2（capturedAt 升序）");
    // 前缀搜索：四姑娘 → 仍命中（末 token 加 * 前缀匹配）
    json::JsonValue prefixParams = json::JsonValue::objectValue();
    prefixParams.setItem("text", "四姑娘");
    rpc::RpcResponse rp = dispatcher.dispatch("library.search", prefixParams);
    check(!rp.isError && rp.result.get("total").asInt() == 2, "case6: 前缀「四姑娘」命中 2 条");
    // 无命中词
    json::JsonValue noneParams = json::JsonValue::objectValue();
    noneParams.setItem("text", "不存在的地名");
    rpc::RpcResponse rn = dispatcher.dispatch("library.search", noneParams);
    check(!rn.isError && rn.result.get("total").asInt() == 0, "case6: 无命中词 → total=0");
  }

  // ---------- 用例 6.5：health.listIssues 分类过滤 + 分页（bindAll 盖位回归） ----------
  // 历史缺陷：listIssues 对同一语句连续两次 bindAll（过滤参数 + 分页参数），
  // bindAll 每次从 ?1 开始绑 → 第二次把 LIMIT 盖到 kind 位、OFFSET 留空 NULL，
  // 表现为 total 正确但 items 恒空（kind='missing' 时 total=362/items=0）。
  {
    std::string hErr;
    health.recordIssue(1, "missing", std::string(kDir) + "/P9000001.RW2",
                       std::string(kDir) + "/P9000001.RW2", &hErr);
    health.recordIssue(2, "missing", std::string(kDir) + "/P9000002.RW2",
                       std::string(kDir) + "/P9000002.RW2", &hErr);
    health.recordIssue(3, "missing", std::string(kDir) + "/P9000003.RW2",
                       std::string(kDir) + "/P9000003.RW2", &hErr);
    health.recordIssue(1, "corrupt", std::string(kDir) + "/P9000001.RW2",
                       std::string(kDir) + "/P9000001.RW2", &hErr);

    json::JsonValue params = json::JsonValue::objectValue();
    params.setItem("kind", std::string("missing"));
    json::JsonValue page = json::JsonValue::objectValue();
    page.setItem("limit", 2);
    page.setItem("offset", 0);
    params.setItem("page", page);
    rpc::RpcResponse r = dispatcher.dispatch("health.listIssues", params);
    check(!r.isError, "case6.5: listIssues 无错误");
    check(r.result.get("total").asInt() == 3, "case6.5: kind=missing total=3");
    const std::vector<json::JsonValue>& items = r.result.get("items").arrayItems();
    check(items.size() == 2, "case6.5: kind=missing limit=2 → 2 条（修复前为 0）");
    bool kindOk = !items.empty();
    for (const json::JsonValue& it : items) {
      if (it.get("kind").asString() != "missing" || it.get("path").asString().empty()) kindOk = false;
    }
    check(kindOk, "case6.5: 每条 kind/path 正确（未被分页参数盖位）");

    json::JsonValue deep = json::JsonValue::objectValue();
    deep.setItem("kind", std::string("missing"));
    json::JsonValue deepPage = json::JsonValue::objectValue();
    deepPage.setItem("limit", 2);
    deepPage.setItem("offset", 2);
    deep.setItem("page", deepPage);
    rpc::RpcResponse rd = dispatcher.dispatch("health.listIssues", deep);
    check(!rd.isError && rd.result.get("total").asInt() == 3 &&
              rd.result.get("items").arrayItems().size() == 1,
          "case6.5: 深分页 offset=2 → 1 条（OFFSET 绑定生效）");

    json::JsonValue corruptParams = json::JsonValue::objectValue();
    corruptParams.setItem("kind", std::string("corrupt"));
    rpc::RpcResponse rc = dispatcher.dispatch("health.listIssues", corruptParams);
    check(!rc.isError && rc.result.get("total").asInt() == 1 &&
              rc.result.get("items").arrayItems().size() == 1,
          "case6.5: kind=corrupt → 1/1（默认分页）");

    json::JsonValue allParams = json::JsonValue::objectValue();
    json::JsonValue allPage = json::JsonValue::objectValue();
    allPage.setItem("limit", 10);
    allPage.setItem("offset", 0);
    allParams.setItem("page", allPage);
    rpc::RpcResponse ra = dispatcher.dispatch("health.listIssues", allParams);
    check(!ra.isError && ra.result.get("total").asInt() == 4 &&
              ra.result.get("items").arrayItems().size() == 4,
          "case6.5: 空 kind → 4/4（两类工单全量）");
  }

  // ---------- 用例 7：时间轴（date=YYYY-MM 分组，返回为数组） ----------
  {
    rpc::RpcResponse r = dispatcher.dispatch("library.getTimeline", json::JsonValue::objectValue());
    check(!r.isError && r.result.isArray(), "case7: library.getTimeline 返回数组");
    const std::vector<json::JsonValue>& months = r.result.arrayItems();
    check(months.size() == 2, "case7: 2 个月份桶（2026-07 / 2026-08）");
    bool found = false;
    for (const json::JsonValue& m : months) {
      if (m.get("month").asString() == "2026-08" && m.get("count").asInt() == 2) found = true;
    }
    check(found, "case7: 2026-08 桶计数=2");
  }

  // ---------- 用例 8：编辑会话 + endSession(flush=true) 四字段（v1.0.1） ----------
  {
    json::JsonValue beginParams = json::JsonValue::objectValue();
    beginParams.setItem("id", 1);
    rpc::RpcResponse rb = dispatcher.dispatch("edit.beginSession", beginParams);
    check(!rb.isError, "case7: edit.beginSession 无错误");
    int64_t sessionId = rb.result.get("sessionId").asInt();
    check(sessionId > 0, "case7: sessionId > 0");

    json::JsonValue setParams = json::JsonValue::objectValue();
    setParams.setItem("sessionId", sessionId);
    json::JsonValue patch = json::JsonValue::objectValue();
    patch.setItem("exposure", 0.65);
    setParams.setItem("patch", patch);
    rpc::RpcResponse rs = dispatcher.dispatch("edit.setParams", setParams);
    check(!rs.isError && rs.result.get("params").get("exposure").asDouble() == 0.65,
          "case7: setParams 深合并 exposure=0.65");

    json::JsonValue endParams = json::JsonValue::objectValue();
    endParams.setItem("sessionId", sessionId);
    endParams.setItem("flush", true);
    rpc::RpcResponse re = dispatcher.dispatch("edit.endSession", endParams);
    check(!re.isError, "case7: endSession(flush=true) 无错误");
    check(re.result.get("flushed").asBool(false) == true, "case7: flushed=true");
    check(re.result.get("params").get("exposure").asDouble() == 0.65, "case7: 返回最终参数");
    check(re.result.get("revision").isInt(), "case7: revision 为 int");
    check(re.result.get("sidecarMtime").isInt(), "case7: sidecarMtime 为 int");
    // sidecar 已真实落盘且含目标字段
    std::string sidecarBytes;
    std::string readErr;
    check(readFile(std::string(kDir) + "/P9000001.xmp", &sidecarBytes, &readErr) &&
              sidecarBytes.find("crs:Exposure2012") != std::string::npos &&
              sidecarBytes.find("0.65") != std::string::npos,
          "case7: sidecar 原子落盘且含 crs:Exposure2012=0.65");
  }

  // ---------- 用例 8：endSession(flush=false) 丢弃未落盘 patch ----------
  {
    json::JsonValue beginParams = json::JsonValue::objectValue();
    beginParams.setItem("id", 2);
    rpc::RpcResponse rb = dispatcher.dispatch("edit.beginSession", beginParams);
    int64_t sessionId = rb.result.get("sessionId").asInt();
    json::JsonValue setParams = json::JsonValue::objectValue();
    setParams.setItem("sessionId", sessionId);
    json::JsonValue patch = json::JsonValue::objectValue();
    patch.setItem("exposure", 2.5);  // 用户显式放弃的值
    setParams.setItem("patch", patch);
    dispatcher.dispatch("edit.setParams", setParams);
    json::JsonValue endParams = json::JsonValue::objectValue();
    endParams.setItem("sessionId", sessionId);
    endParams.setItem("flush", false);
    rpc::RpcResponse re = dispatcher.dispatch("edit.endSession", endParams);
    check(!re.isError, "case8: endSession(flush=false) 无错误");
    check(re.result.get("flushed").asBool(true) == false, "case8: flushed=false");
    check(re.result.get("sidecarMtime").isNull(), "case8: sidecarMtime=null");
    check(re.result.get("params").get("exposure").asDouble() == 2.5,
          "case8: 仍返回内存最终参数");
    // 再次 begin 应从 sidecar 读到旧值（0.65 是上一个会话落盘的），而非被丢弃的 2.5
    rpc::RpcResponse rb2 = dispatcher.dispatch("edit.beginSession", beginParams);
    check(rb2.result.get("params").get("exposure").asDouble() != 2.5,
          "case8: 放弃的 patch 未落盘（begin 读不到 2.5）");
    int64_t sid2 = rb2.result.get("sessionId").asInt();
    json::JsonValue end2 = json::JsonValue::objectValue();
    end2.setItem("sessionId", sid2);
    end2.setItem("flush", false);
    dispatcher.dispatch("edit.endSession", end2);
  }

  // ---------- 用例 9：撤销 / 重做 ----------
  {
    json::JsonValue beginParams = json::JsonValue::objectValue();
    beginParams.setItem("id", 1);
    rpc::RpcResponse rb = dispatcher.dispatch("edit.beginSession", beginParams);
    int64_t sessionId = rb.result.get("sessionId").asInt();
    auto setExposure = [&](double v) {
      json::JsonValue sp = json::JsonValue::objectValue();
      sp.setItem("sessionId", sessionId);
      json::JsonValue patch = json::JsonValue::objectValue();
      patch.setItem("exposure", v);
      sp.setItem("patch", patch);
      return dispatcher.dispatch("edit.setParams", sp);
    };
    setExposure(1.0);
    setExposure(2.0);
    json::JsonValue up = json::JsonValue::objectValue();
    up.setItem("sessionId", sessionId);
    rpc::RpcResponse ru = dispatcher.dispatch("edit.undo", up);
    check(!ru.isError && ru.result.get("params").get("exposure").asDouble() == 1.0,
          "case9: undo 回到 1.0");
    rpc::RpcResponse rr = dispatcher.dispatch("edit.redo", up);
    check(!rr.isError && rr.result.get("params").get("exposure").asDouble() == 2.0,
          "case9: redo 回到 2.0");
    json::JsonValue ep = json::JsonValue::objectValue();
    ep.setItem("sessionId", sessionId);
    ep.setItem("flush", false);
    dispatcher.dispatch("edit.endSession", ep);
  }

  // ---------- 用例 10：结束不存在的会话 → -32602 ----------
  {
    json::JsonValue ep = json::JsonValue::objectValue();
    ep.setItem("sessionId", 999999);
    rpc::RpcResponse re = dispatcher.dispatch("edit.endSession", ep);
    check(re.isError && re.code == rpc::kInvalidParams, "case10: 陌生 sessionId → -32602");
  }

  // ---------- 用例 11：预设种子 ----------
  {
    rpc::RpcResponse r = dispatcher.dispatch("presets.list", json::JsonValue::objectValue());
    check(!r.isError && r.result.isArray() && r.result.arrayItems().size() >= 6,
          "case11: presets.list ≥ 6 个内置预设");
  }

  // ---------- 用例 12：mock.status ----------
  {
    rpc::RpcResponse r = dispatcher.dispatch("mock.status", json::JsonValue::objectValue());
    check(!r.isError, "case12: mock.status 无错误");
    check(r.result.get("assetFormat").asString() == "linear-dng", "case12: assetFormat=linear-dng");
    check(r.result.get("generated").asBool(true) == false, "case12: 尚未生成 → generated=false");
  }

  // ---------- 用例 13：raw.selfTest 对不存在文件优雅失败 ----------
  {
    json::JsonValue params = json::JsonValue::objectValue();
    json::JsonValue ids = json::JsonValue::arrayValue();
    ids.addItem(json::JsonValue::integer(1));  // P9000001.RW2 实际不存在于磁盘
    params.setItem("ids", ids);
    rpc::RpcResponse r = dispatcher.dispatch("raw.selfTest", params);
    check(!r.isError, "case13: selfTest 不崩溃（返回结果而非异常）");
    check(r.result.get("ok").asBool(true) == false, "case13: 文件缺失 → ok=false");
    check(!r.result.get("failures").arrayItems().empty(), "case13: failures 非空（零静默）");
  }

  // ---------- 用例 14：帧编解码（4 字节小端长度前缀） ----------
  {
    std::string payload = "{\"jsonrpc\":\"2.0\",\"id\":1}";
    std::string frame = rpc::encodeFrame(payload);
    check(frame.size() == 4 + payload.size(), "case14: 帧长 = 4 + 载荷");
    uint32_t prefix = static_cast<uint8_t>(frame[0]) | (static_cast<uint8_t>(frame[1]) << 8) |
                      (static_cast<uint8_t>(frame[2]) << 16) |
                      (static_cast<uint8_t>(frame[3]) << 24);
    check(prefix == payload.size(), "case14: 长度前缀为小端 uint32");
    check(frame.compare(4, payload.size(), payload) == 0, "case14: 载荷完整");
    check(rpc::kMaxFrameSize == 8u * 1024u * 1024u, "case14: 单帧上限 8MiB");
  }

  // ---------- 用例 15：settings 合并补丁 ----------
  {
    json::JsonValue params = json::JsonValue::objectValue();
    json::JsonValue patch = json::JsonValue::objectValue();
    patch.setItem("theme", "dark");
    params.setItem("patch", patch);
    rpc::RpcResponse r = dispatcher.dispatch("system.setSettings", params);
    check(!r.isError && r.result.get("settings").get("theme").asString() == "dark",
          "case15: setSettings 深合并生效");
  }

  // ---------- 用例 16（M2 T2.2b / I3）：import.resolveMissingFolder{relocate} 真实路径 ----------
  // 断言（架构 §5 T2.2b）：photos.id 不变 · file_path 已更新 · files.folder_id 指向新
  // 文件夹 · sidecar_path 跟随（手写字符串手术在这四条上全错）。
  {
    // 造一个独立根 + 两个文件夹，照片与 sidecar 都用真实盘上路径
    const std::string rootDir = std::string(kDir) + "/t22b-root";
    const std::string oldRel = "2026/old";
    const std::string newRel = "2026/new";
    makeDirs(rootDir + "/" + oldRel);
    makeDirs(rootDir + "/" + newRel);
    const std::string fileName = "t22b-case16.png";
    // 最小 PNG（1×1 红点）：只需要文件存在，不需要能解码。
    // 放在**新**文件夹（模拟用户已在资源管理器把整个文件夹挪走，DB 还指着旧位置）。
    {
      const unsigned char png[] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A, 0x00, 0x00,
                                   0x00, 0x0D, 'I',  'H',  'D',  'R',  0x00, 0x00, 0x00, 0x01,
                                   0x00, 0x00, 0x00, 0x01, 0x08,  0x02, 0x00, 0x00, 0x00};
      FILE* f = fopen((rootDir + "/" + newRel + "/" + fileName).c_str(), "wb");
      if (f) {
        fwrite(png, 1, sizeof(png), f);
        fclose(f);
      }
    }
    const int64_t rootId = photos.ensureRoot(rootDir, "t22b-root", &err);
    const int64_t folderOld = photos.ensureFolder(rootId, oldRel, "old", &err);
    const int64_t folderNew = photos.ensureFolder(rootId, newRel, "new", &err);
    check(rootId > 0 && folderOld > 0 && folderNew > 0, "case16: root/folder 准备");
    PhotoRecord p16;
    p16.rootId = rootId;
    p16.folderId = folderOld;
    p16.fileName = fileName;
    p16.filePath = rootDir + "/" + oldRel + "/" + fileName;
    p16.format = "PNG";
    p16.importedAt = nowMs();
    p16.sidecarPath = xmp.sidecarPathFor(p16.filePath);
    p16.health = "missing";  // 模拟「整夹不可读」后的照片
    int64_t idOut = 0;
    check(photos.upsert(p16, &idOut, &err), "case16: 插入照片");
    const int64_t photoId16 = idOut;

    json::JsonValue rp = json::JsonValue::objectValue();
    rp.setItem("path", rootDir + "/" + oldRel);
    rp.setItem("action", "relocate");
    rp.setItem("newPath", rootDir + "/" + newRel);
    rpc::RpcResponse r16 = dispatcher.dispatch("import.resolveMissingFolder", rp);
    check(!r16.isError, "case16: resolveMissingFolder 无错误");
    check(r16.result.get("affected").asInt(0) == 1, "case16: affected == 1");

    PhotoRecord after;
    check(photos.getById(photoId16, &after, &err), "case16: 照片仍在（id 未漂移）");
    check(after.filePath == rootDir + "/" + newRel + "/" + fileName,
          "case16: file_path 已更新到新文件夹");
    const int64_t fidNew = imageDao.ensureFileRow(folderNew, fileName, &err);
    check(fidNew > 0 && after.fileId == fidNew,
          "case16: photos.file_id 指向新文件夹的 files 行");
    check(xmp.sidecarPathFor(after.filePath) == after.sidecarPath,
          "case16: sidecar_path 已按新路径重算（不再指向旧文件夹）");
    check(after.fileId != 0, "case16: file_id 非空（身份闭环）");
  }

  // ---------- 用例 17（M2 T2.2b / I3）：health.resolveIssue{relocate} 同四条断言 ----------
  {
    const std::string rootDir = std::string(kDir) + "/t22b-root";
    const std::string fileName = "t22b-case17.png";
    // 第二张照片放在另一个子目录，然后「搬回来」语义：newPath 给到文件级新路径
    const std::string fromRel = "2026/vanish";
    const std::string toRel = "2026/restored";
    makeDirs(rootDir + "/" + toRel);
    {
      const unsigned char png[] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A, 0x00, 0x00,
                                   0x00, 0x0D, 'I',  'H',  'D',  'R',  0x00, 0x00, 0x00, 0x01,
                                   0x00, 0x00, 0x00, 0x01, 0x08,  0x02, 0x00, 0x00, 0x00};
      FILE* f = fopen((rootDir + "/" + toRel + "/" + fileName).c_str(), "wb");
      if (f) {
        fwrite(png, 1, sizeof(png), f);
        fclose(f);
      }
    }
    const int64_t rootId = photos.ensureRoot(rootDir, "t22b-root", &err);
    const int64_t folderFrom = photos.ensureFolder(rootId, fromRel, "vanish", &err);
    const int64_t folderTo = photos.ensureFolder(rootId, toRel, "restored", &err);
    PhotoRecord p17;
    p17.rootId = rootId;
    p17.folderId = folderFrom;
    p17.fileName = fileName;
    p17.filePath = rootDir + "/" + fromRel + "/" + fileName;  // 原路径（已不存在）
    p17.format = "PNG";
    p17.importedAt = nowMs();
    p17.isMissing = true;
    p17.health = "missing";
    int64_t photoId17 = 0;
    check(photos.upsert(p17, &photoId17, &err), "case17: 插入丢失照片");
    check(photos.setMissing(photoId17, true, &err), "case17: 标记 missing");
    // health_issues 行（issueGet 读 photo_id/path/kind）—— 走 DAO，与线上同口径
    const int64_t issueId =
        health.recordIssue(photoId17, "missing", p17.filePath, p17.filePath, &err);
    check(issueId > 0, "case17: 造 health_issue 行");

    json::JsonValue rp = json::JsonValue::objectValue();
    rp.setItem("issueId", json::JsonValue::integer(issueId));
    rp.setItem("action", "relocate");
    rp.setItem("newPath", rootDir + "/" + toRel + "/" + fileName);
    rpc::RpcResponse r17 = dispatcher.dispatch("health.resolveIssue", rp);
    check(!r17.isError, "case17: health.resolveIssue 无错误");
    check(r17.result.get("resolved").asInt(0) == 1, "case17: resolved == 1");

    PhotoRecord after;
    check(photos.getById(photoId17, &after, &err), "case17: 照片仍在（id 未漂移）");
    check(after.filePath == rootDir + "/" + toRel + "/" + fileName,
          "case17: file_path 已更新");
    const int64_t fidTo = imageDao.ensureFileRow(folderTo, fileName, &err);
    check(fidTo > 0 && after.fileId == fidTo, "case17: file_id 指向新文件夹行");
    check(!after.isMissing && after.health == "ok", "case17: missing/health 状态已复位");
    check(xmp.sidecarPathFor(after.filePath) == after.sidecarPath, "case17: sidecar_path 已重算");
  }

  // ---------- 用例 18（M2 T2.2b）：files 表一致性重建幂等 ----------
  {
    db::ImageDao::ReconcileReport rep;
    check(imageDao.reconcile(&rep, &err), "case18: reconcile 成功");
    check(rep.photosUnlinked == 0, "case18: 无无法归位行");
    db::ImageDao::ReconcileReport rep2;
    check(imageDao.reconcile(&rep2, &err), "case18: 二次 reconcile 成功");
    check(rep2.filesInserted == 0 && rep2.photosRelinked == 0,
          "case18: 幂等（二次运行 0 变更）");
  }

  // ---------- 收尾 ----------
  // 顺序要紧：编辑会话必须在 database.close() 之前 shutdown，否则析构里 flush 会撞上
  // 已关闭的 DB（日志里出现 prepare failed: out of memory，纯噪声）。
  sessions.shutdown();
  jobs.shutdown();
  pool.shutdown();
  database.close();

  printf("=== 结果：%s（失败 %d 项） ===\n", gFailures == 0 ? "全部通过" : "存在失败", gFailures);
  fflush(stdout);
  return gFailures == 0 ? 0 : 1;
}
