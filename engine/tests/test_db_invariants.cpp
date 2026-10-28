// v1.2.3 · E4/E5 回归：空数组筛选与 Database 打开期 PRAGMA
//
// E4 缺陷事实：PhotoDao::buildWhere 的 addInts/addStrings 对 values.empty() 仍拼出
//   `col IN ()` —— SQLite 直接语法报错，整个图库查询 500。前端「清空筛选」把
//   folderIds 传成 [] 就必然踩到（core/models.cpp 的 readInts/readStrings 也会把空
//   数组塞进 optional，制造出「已设置但为空」的状态）。
//   正确语义：空数组 = 不筛选（条件不追加），查询结果等于不带该条件。
//
// E5 缺陷事实：Database::open 里四条 PRAGMA 全是 sqlite3_exec(...) 后只
//   sqlite3_free(msg)，rc/msg 一律不看。foreign_keys 没打开会让 files/photos 的
//   ON DELETE 级联静默失效（删照片留孤儿行）。本测试断言打开后实际生效值为 1。
#include <cstdio>
#include <string>
#include <vector>

#include "core/config.h"
#include "core/models.h"
#include "core/str_util.h"
#include "core/thread_pool.h"
#include "db/database.h"
#include "db/photo_dao.h"
#include "json/json_value.h"
#include "library/folder_tree.h"
#include "rpc/dispatcher.h"

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

const char* kHome = "D:/LUMINA/build/runtime/test-db-invariants/home";
const char* kPhotoDir = "D:/LUMINA/build/runtime/test-db-invariants/photos";

}  // namespace

int main() {
  printf("=== E4/E5 · 空数组筛选 + Database PRAGMA 生效值 ===\n");

  makeDirs(kPhotoDir);
  makeDirs(kHome);
  removeFile(std::string(kHome) + "/lumina.db");
  removeFile(std::string(kHome) + "/lumina.db-wal");
  removeFile(std::string(kHome) + "/lumina.db-shm");

  std::string err;
  core::Config config;
  if (!config.init(kHome, &err)) {
    printf("  [FAIL] config.init: %s\n", err.c_str());
    return 1;
  }
  config.load(&err);

  db::Database database;
  if (!database.open(config.dbPath(), &err) || !database.runMigrations(&err)) {
    printf("  [FAIL] database open/migrate: %s\n", err.c_str());
    return 1;
  }

  // ---------------- E5：PRAGMA 实际生效值 ----------------
  printf("\n[E5] Database::open 后 PRAGMA foreign_keys 实际生效值\n");
  {
    auto stmt = database.prepareCached("pragmaFk", "PRAGMA foreign_keys;", &err);
    int fk = -1;
    if (stmt && stmt->valid() && stmt->step(nullptr)) fk = stmt->columnInt(0);
    if (stmt) stmt->reset();
    check(fk == 1, "PRAGMA foreign_keys = 1（实际 " + std::to_string(fk) + "）");

    // 外键真的生效：插一条指向不存在 photo 的 develop_settings 必须被拒
    std::string fkErr;
    const bool inserted = database.exec(
        "INSERT INTO develop_settings(photo_id, params_json, params_hash) "
        "VALUES(999999, '{}', 'x');",
        &fkErr);
    check(!inserted, "外键约束生效：悬空 photo_id 插入被拒（" + fkErr + "）");
  }

  // ---------------- 夹具 ----------------
  printf("\n[夹具] 3 张照片（2×RW2 在 folderA，1×JPG 在 folderB）\n");
  db::PhotoDao photos(&database);
  const int64_t rootId = photos.ensureRoot(kPhotoDir, "test", &err);
  const int64_t folderA = photos.ensureFolder(rootId, "2026/a", "a", &err);
  const int64_t folderB = photos.ensureFolder(rootId, "2026/b", "b", &err);
  check(rootId > 0 && folderA > 0 && folderB > 0, "root/folders 就绪");

  auto makePhoto = [&](const std::string& name, const std::string& format, int64_t folderId) {
    PhotoRecord p;
    p.fileName = name;
    p.filePath = std::string(kPhotoDir) + "/" + name;
    p.format = format;
    p.rootId = rootId;
    p.folderId = folderId;
    p.importedAt = nowMs();
    p.width = 64;
    p.height = 48;
    p.health = "ok";
    int64_t id = 0;
    std::string e;
    if (!photos.upsert(p, &id, &e)) {
      check(false, "upsert " + name + ": " + e);
      return static_cast<int64_t>(0);
    }
    return id;
  };
  const int64_t p1 = makePhoto("a1.RW2", "RW2", folderA);
  const int64_t p2 = makePhoto("a2.RW2", "RW2", folderA);
  const int64_t p3 = makePhoto("b1.JPG", "JPG", folderB);
  check(p1 > 0 && p2 > 0 && p3 > 0, "3 张照片入库");

  // ---------------- E4：空数组 = 不筛选 ----------------
  printf("\n[E4] 空数组筛选不得生成 col IN ()\n");
  {
    Filter none;
    int64_t total = photos.count(none, &err);
    check(total == 3, "无筛选 total=3（实际 " + std::to_string(total) + "）");

    // 空 vector 显式 engaged 的 optional：直接命中 addInts/addStrings 的 empty 分支
    Filter emptyFolders;
    emptyFolders.folderIds = std::vector<int64_t>();
    err.clear();
    int64_t byEmptyFolders = photos.count(emptyFolders, &err);
    check(byEmptyFolders == total,
          "folderIds={} 等价于不筛选（total=" + std::to_string(byEmptyFolders) +
              "，err=" + err + "）");

    Filter emptyFormats;
    emptyFormats.formats = std::vector<std::string>();
    err.clear();
    int64_t byEmptyFormats = photos.count(emptyFormats, &err);
    check(byEmptyFormats == total,
          "formats={} 等价于不筛选（total=" + std::to_string(byEmptyFormats) +
              "，err=" + err + "）");

    Filter emptyBoth;
    emptyBoth.folderIds = std::vector<int64_t>();
    emptyBoth.formats = std::vector<std::string>();
    emptyBoth.cameras = std::vector<std::string>();
    emptyBoth.rootIds = std::vector<int64_t>();
    err.clear();
    const int64_t byAllEmpty = photos.count(emptyBoth, &err);
    check(byAllEmpty == total,
          "四个空数组同时存在也不报错（total=" + std::to_string(byAllEmpty) + "，err=" + err + "）");

    // 非空筛选仍然真的生效（不能被「空数组不筛选」的实现顺手放宽）
    Filter byFolderA;
    byFolderA.folderIds = std::vector<int64_t>{folderA};
    err.clear();
    check(photos.count(byFolderA, &err) == 2,
          "folderIds={A} 仍筛出 2 张（err=" + err + "）");

    Filter byFormatJpg;
    byFormatJpg.formats = std::vector<std::string>{"JPG"};
    err.clear();
    check(photos.count(byFormatJpg, &err) == 1,
          "formats={JPG} 仍筛出 1 张（err=" + err + "）");

    // listIds 走同一 buildWhere：确认空数组也能正常返回全量 id
    Filter emptyForIds;
    emptyForIds.folderIds = std::vector<int64_t>();
    std::vector<int64_t> ids;
    err.clear();
    const bool idsOk = photos.listIds(emptyForIds, &ids, &err);
    check(idsOk && ids.size() == 3,
          "listIds(folderIds={}) 返回 3 条（ok=" + std::string(idsOk ? "1" : "0") + ", n=" +
              std::to_string(ids.size()) + ", err=" + err + "）");
  }

  // ---------------- E4b：filterFromJson 对空数组不置位 ----------------
  printf("\n[E4b] core/models filterFromJson：空数组不设置 optional\n");
  {
    json::JsonValue j = json::JsonValue::objectValue();
    json::JsonValue emptyArr = json::JsonValue::arrayValue();
    j.setItem("folderIds", emptyArr);
    j.setItem("formats", emptyArr);
    Filter f;
    std::string fErr;
    const bool ok = filterFromJson(j, &f, &fErr);
    check(ok, "filterFromJson 解析成功（" + fErr + "）");
    check(!f.folderIds.has_value(), "folderIds=[] → optional 未设置");
    check(!f.formats.has_value(), "formats=[] → optional 未设置");
    int64_t total = photos.count(f, &err);
    check(total == 3, "经 filterFromJson 的空筛选 total=3（实际 " + std::to_string(total) + "）");
  }

  // ---------------- E17：buildFolderTree 不得把 DB 故障当成「图库空了」 ----------------
  printf("\n[E17] buildFolderTree 在 DB 不可用时必须报错而不是静默返回空树\n");
  {
    db::Database notOpen;  // 故意不 open
    db::PhotoDao notOpenPhotos(&notOpen);
    std::string treeErr;
    const std::vector<FolderNode> broken = library::buildFolderTree(&notOpenPhotos, &treeErr);
    check(broken.empty(), "DB 不可用 → 返回空树");
    check(!treeErr.empty(),
          "DB 不可用 → err 非空（不再静默：\"" + treeErr + "\"）");

    // 正常库：err 必须为空且有内容（不能为了修错误路径把正常路径弄坏）
    std::string okErr;
    const std::vector<FolderNode> tree = library::buildFolderTree(&photos, &okErr);
    check(okErr.empty(), "正常库 err 为空（实际 \"" + okErr + "\"）");
    check(tree.size() == 1,
          "正常库返回 1 个库根（实际 " + std::to_string(tree.size()) + "）");
    if (tree.size() == 1) {
      check(tree[0].children.size() == 2,
            "库根下有 2 个文件夹（实际 " + std::to_string(tree[0].children.size()) + "）");
    }
    // err 传 nullptr 也必须安全
    const std::vector<FolderNode> nullErrTree = library::buildFolderTree(&notOpenPhotos, nullptr);
    check(nullErrTree.empty(), "err=nullptr 时安全（不崩溃）");
  }

  printf("\n=== 结果：%s（失败 %d 项）===\n", gFailures == 0 ? "PASS" : "FAIL", gFailures);
  return gFailures == 0 ? 0 : 1;
}
