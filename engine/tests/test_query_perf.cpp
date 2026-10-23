// 查询性能自测（完成定义 #4）：合成 6,000 条记录，
// 验证分页查询 / FTS5 中文搜索 / 多维筛选 / 深分页 / 时间轴均 < 100ms 且结果正确
#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

#include "core/config.h"
#include "core/models.h"
#include "core/str_util.h"
#include "db/database.h"
#include "db/fts_indexer.h"
#include "db/photo_dao.h"
#include "json/json_value.h"
#include "library/query_engine.h"
#include "rpc/dispatcher.h"
#include "rpc/rpc_server.h"

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

// 高精度计时（毫秒）
double nowMsHigh() {
  return std::chrono::duration<double, std::milli>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

const char* kDir = "D:/LUMINA/build/runtime/test-query";
const char* kHome = "D:/LUMINA/build/runtime/test-query/home";
const int kTotal = 6000;

const char* kPlaces[] = {"四姑娘山", "松赞林寺", "雾浓顶", "新都桥", "牛背山",
                         "冷嘎措",   "色达",     "亚丁",   "措普沟", "莲宝叶则"};

}  // namespace

int main() {
  printf("=== 查询性能自测（%d 条记录） ===\n", kTotal);
  makeDirs(kDir);
  removeFile(std::string(kHome) + "/db/lumina.db");
  removeFile(std::string(kHome) + "/db/lumina.db-wal");
  removeFile(std::string(kHome) + "/db/lumina.db-shm");

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
  db::FtsIndexer fts(&database);

  // photos.root_id / photos.folder_id 是外键（PRAGMA foreign_keys=ON），必须先建根与文件夹，
  // 否则单条 upsert 就 FOREIGN KEY constraint failed（0.1 秒内直接失败，不会静默）。
  const int64_t rootId = photos.ensureRoot(kDir, "perf", &err);
  const int64_t folderId = photos.ensureFolder(rootId, "2026", "2026", &err);
  if (rootId <= 0 || folderId <= 0) {
    printf("  [FAIL] 建根目录/文件夹失败: %s\n", err.c_str());
    return 1;
  }

  // ---- 1. 批量灌入（单事务） ----
  double t0 = nowMsHigh();
  database.begin(&err);
  int64_t baseMs = 1767225600000LL;  // 2026-01-01T00:00:00Z
  for (int i = 0; i < kTotal; ++i) {
    PhotoRecord p;
    char name[32];
    snprintf(name, sizeof(name), "P%07d.RW2", i + 1);
    p.fileName = name;
    p.filePath = std::string(kDir) + "/" + name;
    p.rootId = rootId;
    p.folderId = folderId;
    p.format = (i % 10 == 9) ? "JPG" : "RW2";
    p.capturedAt = baseMs + static_cast<int64_t>(i) * 3600000LL * 7;  // 每 7 小时一张
    p.importedAt = baseMs + static_cast<int64_t>(i) * 1000LL;
    p.camera = (i % 4 == 0) ? "DC-S5M2" : "ILCE-7M4";
    p.lens = (i % 3 == 0) ? "LUMIX S 20-60mm F3.5-5.6" : "LUMIX S PRO 24-70mm F2.8";
    p.iso = 100 + (i % 12) * 100;
    p.aperture = "f/2.8";
    p.shutter = "1/125s";
    p.focalLength = "24mm";
    p.location = kPlaces[i % 10];  // FTS 列
    char month[8];
    int monthIdx = (i * 7 / 24 / 30) % 9;  // 2026-01 .. 2026-09
    snprintf(month, sizeof(month), "2026-%02d", monthIdx + 1);
    p.date = month;
    p.rating = (i % 23 == 0) ? 4 : (i % 17 == 0 ? 3 : (i % 7 == 0 ? 2 : 0));
    p.width = 160;
    p.height = 112;
    p.health = "ok";
    p.sidecarPath = replaceExtension(p.filePath, "xmp");
    int64_t id = 0;
    if (!photos.upsert(p, &id, &err)) {
      printf("  [FAIL] upsert #%d: %s\n", i, err.c_str());
      return 1;
    }
  }
  database.commit(&err);
  double insertMs = nowMsHigh() - t0;
  printf("  [INFO] 灌入 %d 条耗时 %.0f ms\n", kTotal, insertMs);

  t0 = nowMsHigh();
  fts.rebuild(&err);
  printf("  [INFO] FTS rebuild 耗时 %.0f ms\n", nowMsHigh() - t0);

  library::QueryEngine query(&config, &database, &photos);
  rpc::RpcError rpcErr;

  // ---- 2. 分页查询（首页 100 条） ----
  auto runQuery = [&](const json::JsonValue& params) {
    return query.queryPhotos(params, &rpcErr);
  };
  {
    json::JsonValue params = json::JsonValue::objectValue();
    json::JsonValue page = json::JsonValue::objectValue();
    page.setItem("limit", 100);
    page.setItem("offset", 0);
    params.setItem("page", page);
    // 预热
    json::JsonValue warm = runQuery(params);
    check(!warm.get("items").arrayItems().empty(), "warmup: queryPhotos 有结果");
    double worst = 0.0;
    json::JsonValue r;
    for (int i = 0; i < 3; ++i) {
      double s = nowMsHigh();
      r = runQuery(params);
      double d = nowMsHigh() - s;
      if (d > worst) worst = d;
    }
    check(r.get("total").asInt() == kTotal, "A: total=6000");
    check(r.get("items").arrayItems().size() == 100, "A: 首页 100 条");
    check(worst < 100.0, "A: 分页查询最差耗时 " + std::to_string(worst) + " ms < 100ms");
    printf("  [INFO] A 分页查询最差耗时 %.2f ms\n", worst);
  }

  // ---- 3. FTS5 中文搜索 ----
  {
    json::JsonValue params = json::JsonValue::objectValue();
    params.setItem("text", "四姑娘山");
    json::JsonValue page = json::JsonValue::objectValue();
    page.setItem("limit", 100);
    params.setItem("page", page);
    json::JsonValue warm = query.search(params, &rpcErr);
    check(warm.get("total").asInt() == kTotal / 10, "warmup: 「四姑娘山」命中 600");
    double worst = 0.0;
    json::JsonValue r;
    for (int i = 0; i < 3; ++i) {
      double s = nowMsHigh();
      r = query.search(params, &rpcErr);
      double d = nowMsHigh() - s;
      if (d > worst) worst = d;
    }
    check(r.get("total").asInt() == kTotal / 10, "B: 搜索「四姑娘山」命中 600 条");
    check(worst < 100.0, "B: 中文搜索最差耗时 " + std::to_string(worst) + " ms < 100ms");
    printf("  [INFO] B FTS5 中文搜索最差耗时 %.2f ms\n", worst);
    // 无命中词
    json::JsonValue noneParams = json::JsonValue::objectValue();
    noneParams.setItem("text", "喀纳斯");
    json::JsonValue rn = query.search(noneParams, &rpcErr);
    check(rn.get("total").asInt() == 0, "B: 无命中词 → 0 条");
  }

  // ---- 4. 多维筛选 + 排序 ----
  {
    json::JsonValue params = json::JsonValue::objectValue();
    json::JsonValue filter = json::JsonValue::objectValue();
    filter.setItem("minRating", 3);
    params.setItem("filter", filter);
    json::JsonValue sort = json::JsonValue::objectValue();
    sort.setItem("field", "rating");
    sort.setItem("order", "desc");
    params.setItem("sort", sort);
    json::JsonValue page = json::JsonValue::objectValue();
    page.setItem("limit", 200);
    params.setItem("page", page);
    json::JsonValue warm = runQuery(params);
    check(warm.get("total").asInt() > 0, "warmup: 评分≥3 有结果");
    double worst = 0.0;
    json::JsonValue r;
    for (int i = 0; i < 3; ++i) {
      double s = nowMsHigh();
      r = runQuery(params);
      double d = nowMsHigh() - s;
      if (d > worst) worst = d;
    }
    // 校验降序：首条评分 ≥ 末条评分
    const std::vector<json::JsonValue>& items = r.get("items").arrayItems();
    bool ordered = true;
    for (size_t i = 1; i < items.size(); ++i) {
      if (items[i - 1].get("rating").asInt() < items[i].get("rating").asInt()) ordered = false;
    }
    check(ordered, "C: 按评分降序返回");
    check(worst < 100.0, "C: 筛选+排序最差耗时 " + std::to_string(worst) + " ms < 100ms");
    printf("  [INFO] C 筛选+排序最差耗时 %.2f ms（命中 %lld 条）\n", worst,
           static_cast<long long>(r.get("total").asInt()));
  }

  // ---- 5. 深分页 ----
  {
    json::JsonValue params = json::JsonValue::objectValue();
    json::JsonValue page = json::JsonValue::objectValue();
    page.setItem("limit", 100);
    page.setItem("offset", kTotal - 100);
    params.setItem("page", page);
    double worst = 0.0;
    json::JsonValue r;
    for (int i = 0; i < 3; ++i) {
      double s = nowMsHigh();
      r = runQuery(params);
      double d = nowMsHigh() - s;
      if (d > worst) worst = d;
    }
    check(r.get("items").arrayItems().size() == 100, "D: 深分页返回 100 条");
    check(worst < 100.0, "D: 深分页最差耗时 " + std::to_string(worst) + " ms < 100ms");
    printf("  [INFO] D 深分页最差耗时 %.2f ms\n", worst);
  }

  // ---- 6. 时间轴 ----
  {
    double worst = 0.0;
    json::JsonValue r;
    for (int i = 0; i < 3; ++i) {
      double s = nowMsHigh();
      r = query.getTimeline(json::JsonValue::objectValue(), &rpcErr);
      double d = nowMsHigh() - s;
      if (d > worst) worst = d;
    }
    check(r.isArray() && r.arrayItems().size() == 9, "E: 9 个月份桶");
    int64_t sum = 0;
    for (const json::JsonValue& m : r.arrayItems()) sum += m.get("count").asInt();
    check(sum == kTotal, "E: 月份桶计数合计 = 6000");
    check(worst < 100.0, "E: 时间轴最差耗时 " + std::to_string(worst) + " ms < 100ms");
    printf("  [INFO] E 时间轴最差耗时 %.2f ms\n", worst);
  }

  database.close();
  printf("=== 结果：%s（失败 %d 项） ===\n", gFailures == 0 ? "全部通过" : "存在失败", gFailures);
  return gFailures == 0 ? 0 : 1;
}
