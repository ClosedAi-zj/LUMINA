// M2 T2.12 · migration 005 repair_sidecar_paths 自测（N35，架构 §3.3.1 末小节）
//
// 核心命题：存量 photos.sidecar_path 失效行必须被一次性修复，且全程无静默——
//   修复的三条出路（判定 sidecar_path<>'' 且 !fileExists）：
//     a) 重算命中：replaceExtension(file_path,"xmp") 存在 → UPDATE 路径+mtime；
//     b) 归零：重算不存在且 has_edits=0 → 本来无 sidecar（导入时写下的虚构路径），置 ''；
//     c) 不可恢复：重算不存在且 has_edits=1（sidecar 真丢失）→ 保留原路径 +
//        逐条进 unrecoverable[]（不清空、不静默丢弃）。
// 覆盖：
//   S1  判定与修复：有效/重算命中/归零/不可恢复/空串/NULL 六类行的分流
//   S2  幂等：第二遍 repaired=0、unrecoverable 集合不变（不动点）
//   S3  无静默：修复后「失效且不在 unrecoverable 名单」的行数 == 0
//   S4  报告产物：{home}/reports/m2-sidecar-repair.json 可解析、计数一致
//   S5  版本门控：db_version=5 后 runVersionedMigrations 不再重复执行 005
#include <cstdio>
#include <string>
#include <vector>

#include "core/str_util.h"
#include "db/database.h"
#include "db/migrations.h"
#include "json/json_value.h"

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

const char* kHome = "D:/LUMINA/build/runtime/test-sidecar-repair/home";
const char* kPhotos = "D:/LUMINA/build/runtime/test-sidecar-repair/photos";
const char* kOldDir = "D:/LUMINA/build/runtime/test-sidecar-repair/moved-away";

int querySeq = 0;

// 单值查询
int64_t scalar(db::Database* db, const std::string& sql) {
  auto stmt = db->prepareCached("t212q" + std::to_string(++querySeq), sql, nullptr);
  if (!stmt || !stmt->valid()) return -1;
  int64_t v = -1;
  if (stmt->step(nullptr)) v = stmt->columnInt64(0);
  stmt->reset();
  return v;
}

std::string text(db::Database* db, const std::string& sql) {
  auto stmt = db->prepareCached("t212t" + std::to_string(++querySeq), sql, nullptr);
  if (!stmt || !stmt->valid()) return "<query-failed>";
  std::string v = "<no-row>";
  if (stmt->step(nullptr)) v = stmt->columnText(0);
  stmt->reset();
  return v;
}

void seedPhoto(db::Database* db, const std::string& name, const char* sidecarPath,
               int64_t sidecarMtime, int hasEdits) {
  std::string e;
  auto stmt = db->prepareCached("t212seed" + std::to_string(++querySeq),
                                "INSERT INTO photos(file_name, file_path, sidecar_path, "
                                "sidecar_mtime, has_edits) VALUES(?,?,?,?,?);",
                                &e);
  stmt->bindText(1, name);
  stmt->bindText(2, std::string(kPhotos) + "/" + name);
  if (sidecarPath == nullptr) {
    stmt->bindNull(3);
  } else {
    stmt->bindText(3, sidecarPath);
  }
  if (sidecarMtime < 0) {
    stmt->bindNull(4);
  } else {
    stmt->bindInt64(4, sidecarMtime);
  }
  stmt->bindInt64(5, hasEdits);
  stmt->step(nullptr);
}

}  // namespace

int main() {
  printf("=== migration 005 repair_sidecar_paths 自测（T2.12 / S1-S5）===\n");

  // 清理上次残留
  removeFile(std::string(kHome) + "/lumina.db");
  removeFile(std::string(kHome) + "/lumina.db-wal");
  removeFile(std::string(kHome) + "/lumina.db-shm");

  makeDirs(kPhotos);
  makeDirs(kHome);

  // 夹具文件：
  //   p1.jpg + p1.xmp  —— 有效行（sidecar_path 指向存在的文件）
  //   p2.jpg + p2.xmp  —— 重算命中（DB 记的是移动前的旧路径，重算后存在）
  //   p3.jpg           —— sidecar 真丢失（has_edits=1，哪里都没有 xmp）
  //   p4.jpg           —— 虚构路径（has_edits=0，importer 写下的 .xmp 从未存在）
  const std::string p1xmp = std::string(kPhotos) + "/p1.xmp";
  const std::string p2xmp = std::string(kPhotos) + "/p2.xmp";
  const std::string staleP2 = std::string(kOldDir) + "/p2.xmp";       // 不存在
  const std::string staleP3 = std::string(kOldDir) + "/p3.xmp";       // 不存在
  const std::string staleP4 = std::string(kPhotos) + "/p4.xmp";       // 不存在
  writeFileBytes(p1xmp, "<xmp p1>", nullptr);
  writeFileBytes(p2xmp, "<xmp p2>", nullptr);

  std::string err;
  db::Database database;
  if (!database.open(std::string(kHome) + "/lumina.db", &err)) {
    printf("  [FAIL] database.open: %s\n", err.c_str());
    return 1;
  }
  // 空库走完整迁移链：schema + 002..005（空表上 005 scanned=0，顺带验证链路）
  if (!database.runMigrations(&err)) {
    printf("  [FAIL] runMigrations: %s\n", err.c_str());
    return 1;
  }
  check(database.schemaVersion() == db::kTargetDbVersion,
        "db_version = kTargetDbVersion = " + std::to_string(db::kTargetDbVersion));

  // 播种七类行（在初始迁移之后：005 可独立重入，与 002 同模式）
  seedPhoto(&database, "p1.jpg", p1xmp.c_str(), 11111, 1);        // 有效
  seedPhoto(&database, "p2.jpg", staleP2.c_str(), 22222, 1);      // 重算命中
  seedPhoto(&database, "p3.jpg", staleP3.c_str(), 33333, 1);      // 不可恢复
  seedPhoto(&database, "p4.jpg", staleP4.c_str(), -1, 0);         // 归零
  seedPhoto(&database, "p5.jpg", "", -1, 0);                      // 空串 = 本来无 sidecar
  seedPhoto(&database, "p6.jpg", nullptr, -1, 0);                 // NULL 同上
  seedPhoto(&database, "p7.jpg", staleP4.c_str(), 44444, 1);      // 不可恢复第二条（多行逐条列出）

  const std::string reportPath = std::string(kHome) + "/reports/m2-sidecar-repair.json";

  // ---------------- S1 判定与修复分流 ----------------
  printf("\n[S1] 判定与修复分流\n");
  db::SidecarRepairReport rep1;
  int code = db::kMigrationOk;
  check(db::applyMigration005RepairSidecarPaths(&database, &rep1, &code, reportPath),
        "第一遍 applyMigration005 成功");
  check(rep1.scanned == 5, "scanned=5（空串/NULL 行不进扫描）实际=" +
                               std::to_string(rep1.scanned));
  check(rep1.stale == 4, "stale=4（p2/p3/p4/p7 失效）实际=" + std::to_string(rep1.stale));
  check(rep1.repaired == 2 && rep1.repairedByRecompute == 1 && rep1.normalized == 1,
        "repaired=2（重算 1 + 归零 1）实际=" + std::to_string(rep1.repaired) + "/" +
            std::to_string(rep1.repairedByRecompute) + "/" + std::to_string(rep1.normalized));
  check(rep1.unrecoverable.size() == 2 &&
            rep1.unrecoverable[0].photoId > 0 &&
            rep1.unrecoverable[0].filePath == std::string(kPhotos) + "/p3.jpg" &&
            rep1.unrecoverable[0].triedSidecar == std::string(kPhotos) + "/p3.xmp" &&
            rep1.unrecoverable[1].filePath == std::string(kPhotos) + "/p7.jpg",
        "unrecoverable 恰 2 条（p3/p7 逐条列出，字段齐全）");
  check(text(&database, "SELECT sidecar_path FROM photos WHERE file_name='p2.jpg';") == p2xmp,
        "p2 sidecar_path 已重算为新位置");
  check(scalar(&database, "SELECT sidecar_mtime FROM photos WHERE file_name='p2.jpg';") ==
            fileMtimeMs(p2xmp),
        "p2 sidecar_mtime 已刷成本地文件 mtime");
  check(text(&database, "SELECT sidecar_path FROM photos WHERE file_name='p3.jpg';") == staleP3,
        "p3 sidecar_path 保留原状（不清空线索）");
  check(text(&database, "SELECT sidecar_path FROM photos WHERE file_name='p4.jpg';") == "",
        "p4 虚构路径已归零（has_edits=0 本来无 sidecar）");
  check(text(&database, "SELECT sidecar_path FROM photos WHERE file_name='p1.jpg';") == p1xmp,
        "p1 有效行未被触碰");

  // ---------------- S3 无静默 ----------------
  printf("\n[S3] 无静默：失效行要么修复要么在 unrecoverable 名单\n");
  // 逐条核对：修复后仍失效的行（sidecar 非空且文件不存在）必须全部出现在 rep1.unrecoverable
  int remainingStale = 0;
  int notListed = 0;
  {
    auto stmt = database.prepareCached(
        "t212stale",
        "SELECT id, sidecar_path FROM photos WHERE sidecar_path IS NOT NULL AND sidecar_path != '';",
        nullptr);
    struct Row {
      int64_t id;
      std::string path;
    };
    std::vector<Row> rows;
    while (stmt->step(nullptr)) rows.push_back({stmt->columnInt64(0), stmt->columnText(1)});
    stmt->reset();
    for (const Row& r : rows) {
      if (fileExists(r.path)) continue;  // 有效行（p1/p2），不算失效
      remainingStale++;
      bool listed = false;
      for (const auto& u : rep1.unrecoverable) {
        if (u.photoId == r.id) {
          listed = true;
          break;
        }
      }
      if (!listed) notListed++;
    }
  }
  check(remainingStale == static_cast<int>(rep1.unrecoverable.size()),
        "残余失效行数 == unrecoverable 条数（" + std::to_string(remainingStale) + "）");
  check(notListed == 0, "残余失效行全部在 unrecoverable 名单（未列出的=" +
                            std::to_string(notListed) + "）");

  // ---------------- S2 幂等 ----------------
  printf("\n[S2] 幂等：第二遍不动点\n");
  db::SidecarRepairReport rep2;
  check(db::applyMigration005RepairSidecarPaths(&database, &rep2, &code), "第二遍成功");
  check(rep2.scanned == rep1.scanned - rep1.normalized,
        "第二遍 scanned = 第一遍 - 归零行（归零行已离开非空集合）实际=" +
            std::to_string(rep2.scanned));
  check(rep2.repaired == 0 && rep2.normalized == 0 && rep2.repairedByRecompute == 0,
        "第二遍 repaired=0（实际=" + std::to_string(rep2.repaired) + "）");
  check(rep2.unrecoverable.size() == rep1.unrecoverable.size() &&
            rep2.unrecoverable.size() == 2 &&
            rep2.unrecoverable[0].photoId == rep1.unrecoverable[0].photoId &&
            rep2.unrecoverable[0].triedSidecar == rep1.unrecoverable[0].triedSidecar &&
            rep2.unrecoverable[1].photoId == rep1.unrecoverable[1].photoId,
        "第二遍 unrecoverable 与第一遍逐条一致（集合不动点）");
  check(rep2.stale == static_cast<int64_t>(rep2.unrecoverable.size()),
        "第二遍 stale == unrecoverable 条数（修复后只剩真丢失行）");

  // ---------------- S4 报告产物 ----------------
  printf("\n[S4] 报告产物\n");
  {
    std::string bytes;
    json::JsonValue root;
    const bool readOk = readFile(reportPath, &bytes, &err) &&
                        json::JsonValue::parse(bytes, &root, &err);
    check(readOk, "报告 JSON 可读取可解析（" + reportPath + "）");
    check(readOk && root.get("scanned").asInt() == rep1.scanned &&
              root.get("stale").asInt() == rep1.stale &&
              root.get("repaired").asInt() == rep1.repaired &&
              root.get("unrecoverableCount").asInt() ==
                  static_cast<int64_t>(rep1.unrecoverable.size()) &&
              root.get("unrecoverable").size() == rep1.unrecoverable.size(),
          "报告计数与内存 report 一致（unrecoverable 条数 == 报告条数，防漏记）");
    check(readOk && root.get("unrecoverable").at(0).get("filePath").asString() ==
                        rep1.unrecoverable[0].filePath,
          "报告 unrecoverable 条目含 filePath（用户手工找回入口）");
  }

  // ---------------- S5 版本门控 ----------------
  printf("\n[S5] 版本门控\n");
  {
    db::MigrationReport mrep;
    check(db::runVersionedMigrations(&database, &code, &mrep), "runVersionedMigrations 成功");
    check(mrep.upToDate && mrep.statementsRun == 0,
          "db_version=5 后整体跳过（005 不随链重复执行）");
  }

  printf("\n%s（failures=%d）\n", gFailures == 0 ? "=== ALL PASS ===" : "=== FAILED ===",
         gFailures);
  return gFailures == 0 ? 0 : 1;
}
