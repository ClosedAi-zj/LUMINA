// M2 T2.2 验收：四级表迁移幂等 + 图像身份稳定
//
// 断言清单（架构 §5 T2.2 验收 + §3.3.1 语义表）：
//   I1 回填后每行 photos 都有 file_id，files 行按 (folder_id, name) 唯一；
//   I2 迁移幂等：二次执行 0 变更（filesInserted == 0 && photosLinked == 0）；
//   I3 移动/重命名文件夹后 photos.id 不变（回放测试：先记录 id，再搬迁，再按新路径解析）；
//   I4 photos_fts 仍可检索（迁移没有重建 photos 表，触发器仍然生效）；
//   I5 旧行（迁移前插入、file_id 为空）能被 resolveByFile 惰性补齐绑定；
//   I6 relocate 的冲突路径必须显式失败（不静默覆盖别人的 file_path）。
#include <cstdio>
#include <string>
#include <vector>

#include "core/models.h"
#include "core/str_util.h"
#include "db/database.h"
#include "db/image_dao.h"
#include "db/migrations.h"
#include "db/photo_dao.h"

using namespace lumina;
using lumina::db::Database;
using lumina::db::ImageDao;
using lumina::db::MigrationReport;
using lumina::db::PhotoDao;

static int g_fail = 0;
#define CHECK(cond, msg)                                                 \
  do {                                                                   \
    if (!(cond)) {                                                       \
      std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, msg);           \
      ++g_fail;                                                          \
    } else {                                                             \
      std::printf("  [PASS] %s\n", msg);                                 \
    }                                                                    \
  } while (0)

namespace {

int64_t scalarInt(Database* db, const std::string& sql) {
  auto stmt = db->prepareCached("t:" + sql, sql, nullptr);
  if (!stmt || !stmt->valid()) return -1;
  int64_t v = -1;
  if (stmt->step(nullptr)) v = stmt->columnInt64(0);
  stmt->reset();
  return v;
}

// 插入一条照片（file_id 由迁移回填，这里不写）
int64_t insertPhoto(PhotoDao* dao, int64_t rootId, int64_t folderId, const std::string& rootPath,
                    const std::string& relPath, const std::string& name, const std::string& loc,
                    const std::string& date) {
  PhotoRecord p;
  p.folderId = folderId;
  p.rootId = rootId;
  p.fileName = name;
  p.filePath = relPath.empty() ? joinPath(rootPath, name) : joinPath(joinPath(rootPath, relPath), name);
  p.format = "RW2";
  p.location = loc;
  p.date = date;
  int64_t id = 0;
  std::string err;
  if (!dao->upsert(p, &id, &err)) {
    std::printf("    upsert failed: %s\n", err.c_str());
    return 0;
  }
  return id;
}

// 把库版本倒回 1，用于重放 002（模拟 M1 存量库 / 二次启动）
void resetVersionTo1(Database* db) {
  std::string err;
  db->exec("UPDATE schema_meta SET value='1' WHERE key='db_version';", &err);
}

}  // namespace

int main() {
  const std::string dbPath = "lumina_test_image_identity.db";
  removeFile(dbPath);
  removeFile(dbPath + "-wal");
  removeFile(dbPath + "-shm");

  Database db;
  std::string err;
  if (!db.open(dbPath, &err)) {
    std::printf("FATAL open: %s\n", err.c_str());
    return 2;
  }
  if (!db.runMigrations(&err)) {
    std::printf("FATAL runMigrations: %s\n", err.c_str());
    return 2;
  }
  std::printf("migrate: db_version=%d（期望 %d）\n", db.schemaVersion(), lumina::db::kTargetDbVersion);
  CHECK(db.schemaVersion() == lumina::db::kTargetDbVersion, "runMigrations 写入 db_version=2");
  CHECK(lumina::db::tableHasColumn(&db, "photos", "file_id"), "photos.file_id 列已存在");
  CHECK(lumina::db::tableHasColumn(&db, "files", "name"), "files 表已建（四级表）");

  // ---- 造一批「M1 存量」数据：photos 有行但 file_id 为空 ----
  PhotoDao photos(&db);
  ImageDao images(&db);
  const std::string rootPath = "D:/library";
  const int64_t rootId = photos.ensureRoot(rootPath, "library", &err);
  CHECK(rootId > 0, "ensureRoot");
  const int64_t folderA = photos.ensureFolder(rootId, "2026/风光", "风光", &err);
  const int64_t folderB = photos.ensureFolder(rootId, "2026/建筑", "建筑", &err);
  CHECK(folderA > 0 && folderB > 0, "ensureFolder ×2");

  const int64_t p1 = insertPhoto(&photos, rootId, folderA, rootPath, "2026/风光", "a001.rw2",
                                 "贡嘎雪山", "2026-05");
  const int64_t p2 = insertPhoto(&photos, rootId, folderA, rootPath, "2026/风光", "a002.rw2",
                                 "贡嘎日照金山", "2026-05");
  const int64_t p3 = insertPhoto(&photos, rootId, folderB, rootPath, "2026/建筑", "b001.rw2",
                                 "外滩夜景", "2026-06");
  CHECK(p1 > 0 && p2 > 0 && p3 > 0, "插入 3 张照片（file_id 为空）");
  CHECK(scalarInt(&db, "SELECT COUNT(*) FROM photos WHERE file_id IS NULL;") == 3,
        "迁移前 file_id 全为空");

  // ---- 重放 002（模拟 M1 存量库升级） ----
  resetVersionTo1(&db);
  MigrationReport rep;
  int migErr = 0;
  CHECK(lumina::db::runVersionedMigrations(&db, &migErr, &rep), "002 迁移执行成功");
  std::printf("    002: from=%d to=%d files+%d linked=%d unlinked=%d\n", rep.fromVersion,
              rep.toVersion, rep.filesInserted, rep.photosLinked, rep.photosUnlinked);
  CHECK(rep.filesInserted == 3, "回填新增 3 行 files");
  CHECK(rep.photosLinked == 3, "回填关联 3 行 photos.file_id");
  CHECK(rep.photosUnlinked == 0, "无无法归位的行");
  CHECK(scalarInt(&db, "SELECT COUNT(*) FROM photos WHERE file_id IS NULL;") == 0,
        "回填后 file_id 全部非空");
  CHECK(scalarInt(&db, "SELECT COUNT(*) FROM files;") == 3, "files 行数 = 3");

  // ---- I2 幂等：二次执行 0 变更 ----
  resetVersionTo1(&db);
  MigrationReport rep2;
  CHECK(lumina::db::runVersionedMigrations(&db, &migErr, &rep2), "002 二次执行成功");
  std::printf("    002(second): files+%d linked=%d statements=%d\n", rep2.filesInserted,
              rep2.photosLinked, rep2.statementsRun);
  CHECK(rep2.filesInserted == 0, "幂等：二次执行 filesInserted == 0");
  CHECK(rep2.photosLinked == 0, "幂等：二次执行 photosLinked == 0");
  CHECK(scalarInt(&db, "SELECT COUNT(*) FROM files;") == 3, "幂等：files 行数仍为 3");

  // ---- 版本已达标时整体跳过 ----
  MigrationReport rep3;
  CHECK(lumina::db::runVersionedMigrations(&db, &migErr, &rep3), "版本达标时调用成功");
  CHECK(rep3.upToDate && rep3.filesInserted == 0 && rep3.photosLinked == 0,
        "版本达标 → upToDate 且 0 变更");

  // ---- I4 FTS 仍可检索（迁移没重建 photos 表） ----
  CHECK(scalarInt(&db, "SELECT COUNT(*) FROM photos_fts WHERE photos_fts MATCH '贡嘎*';") == 2,
        "photos_fts 迁移后仍可检索（'贡嘎' 命中 2）");
  CHECK(scalarInt(&db, "SELECT COUNT(*) FROM photos_fts WHERE photos_fts MATCH '外滩*';") == 1,
        "photos_fts 命中 '外滩' 1");

  // ---- I1 文件身份：ensureFileRow 幂等 ----
  const int64_t f1a = images.ensureFileRow(folderA, "a001.rw2", &err);
  const int64_t f1b = images.ensureFileRow(folderA, "a001.rw2", &err);
  CHECK(f1a > 0 && f1a == f1b, "ensureFileRow 幂等（同 id）");
  CHECK(images.fileIdOfPhoto(p1) == f1a, "photos.file_id 指向 files(folder,name)");
  CHECK(scalarInt(&db, "SELECT COUNT(*) FROM files WHERE folder_id=" + std::to_string(folderA) +
                           " AND name='a001.rw2';") == 1,
        "files(folder_id,name) 唯一");

  // ---- I3 回放：移动/重命名文件夹后 photos.id 不变 ----
  // ① 重命名：2026/风光 → 2026/landscape（只改 folders.rel_path + photos.file_path）
  const int64_t idBefore = p1;
  const int64_t fileIdBefore = images.fileIdOfPhoto(p1, &err);
  {
    std::string e;
    CHECK(db.begin(&e), "begin");
    CHECK(db.exec("UPDATE folders SET rel_path='2026/landscape', name='landscape' WHERE id=" +
                      std::to_string(folderA) + ";",
                  &e),
          "重命名文件夹（rel_path）");
    CHECK(db.exec("UPDATE photos SET file_path=replace(file_path,'2026/风光','2026/landscape') "
                  "WHERE folder_id=" +
                      std::to_string(folderA) + ";",
                  &e),
          "同步 photos.file_path");
    CHECK(db.commit(&e), "commit");
  }
  const int64_t resolved1 = images.resolveByFile(rootId, "2026/landscape", "a001.rw2", &err);
  std::printf("    resolve(2026/landscape, a001.rw2) = %lld（搬迁前 id=%lld）\n",
              static_cast<long long>(resolved1), static_cast<long long>(idBefore));
  CHECK(resolved1 == idBefore, "I3：重命名文件夹后按新路径解析回同一 photos.id");
  CHECK(images.fileIdOfPhoto(p1, &err) == fileIdBefore, "I3：files.id 也未变");

  // ② 移动：2026/landscape → 2027/风光（ImageDao::relocate）
  CHECK(images.relocate(p1, rootId, "2027/风光", &err), "relocate 到 2027/风光");
  CHECK(scalarInt(&db, "SELECT COUNT(*) FROM photos WHERE id=" + std::to_string(idBefore) + ";") == 1,
        "I3：relocate 后 photos.id 仍存在（未删旧行建新行）");
  CHECK(images.fileIdOfPhoto(p1, &err) > 0, "I3：relocate 后仍绑定 file_id");
  const int64_t resolved2 = images.resolveByFile(rootId, "2027/风光", "a001.rw2", &err);
  CHECK(resolved2 == idBefore, "I3：移动文件夹后按新路径解析回同一 photos.id");
  CHECK(images.resolveByFile(rootId, "2026/landscape", "a001.rw2", &err) == 0,
        "I3：旧路径不再命中（搬迁是移动而非复制）");
  // 路径列确实更新了（id 稳定不等于路径没动）
  {
    auto stmt = db.prepareCached("t:path", "SELECT file_path FROM photos WHERE id=?", &err);
    if (stmt && stmt->valid()) {
      stmt->bindInt64(1, p1);
      if (stmt->step(&err)) {
        const std::string path = stmt->columnText(0);
        std::printf("    new file_path = %s\n", path.c_str());
        CHECK(path.find("2027/风光") != std::string::npos, "file_path 已随搬迁更新");
      }
      stmt->reset();
    }
  }

  // ---- I4 FTS 在搬迁后仍可检索 ----
  CHECK(scalarInt(&db, "SELECT COUNT(*) FROM photos_fts WHERE photos_fts MATCH '贡嘎*';") == 2,
        "photos_fts 搬迁后仍命中 '贡嘎' 2");

  // ---- I5 旧行惰性补齐：造一条 file_id 为空的旧行，resolveByFile 应补齐 ----
  {
    const int64_t legacy = insertPhoto(&photos, rootId, folderB, rootPath, "2026/建筑", "b002.rw2",
                                       "陆家嘴", "2026-06");
    CHECK(legacy > 0, "插入旧行（file_id 为空）");
    CHECK(scalarInt(&db, "SELECT COUNT(*) FROM photos WHERE id=" + std::to_string(legacy) +
                             " AND file_id IS NULL;") == 1,
          "旧行 file_id 为空");
    const int64_t got = images.resolveByFile(rootId, "2026/建筑", "b002.rw2", &err);
    CHECK(got == legacy, "I5：resolveByFile 命中旧行");
    CHECK(scalarInt(&db, "SELECT COUNT(*) FROM photos WHERE id=" + std::to_string(legacy) +
                             " AND file_id IS NOT NULL;") == 1,
          "I5：命中后顺手补齐 file_id（零静默：不留半绑定行）");
  }

  // ---- I6 relocate 冲突必须显式失败（不静默覆盖别人的 file_path） ----
  {
    const int64_t p2Id = p2;
    std::string e;
    // 让 p2 的文件名与 p3 相同（两者在不同文件夹 → file_path 仍唯一）
    CHECK(db.exec("UPDATE photos SET file_name='b001.rw2' WHERE id=" + std::to_string(p2Id) + ";",
                  &e),
          "I6 前置：p2 改名为 b001.rw2（不同文件夹，file_path 仍唯一）");
    std::string conflictErr;
    const bool conflictOk = images.relocate(p2Id, rootId, "2026/建筑", &conflictErr);
    std::printf("    conflict relocate → ok=%d err=%s\n", conflictOk ? 1 : 0,
                conflictErr.c_str());
    CHECK(!conflictOk && !conflictErr.empty(), "I6：目标 file_path 冲突时显式失败并给出原因");
    // 冲突时不得留下半搬迁状态：p2 的 folder 归属必须仍是原文件夹（folderA）
    // 严格断言（QA 指出 != folderB 是弱断言：folder 归属被改去任何第三个文件夹也算过）
    CHECK(scalarInt(&db, "SELECT folder_id FROM photos WHERE id=" + std::to_string(p2Id) + ";") ==
              folderA,
          "I6：冲突失败后 folder 归属仍是原文件夹（== folderA）");
  }

  db.close();
  removeFile(dbPath);
  removeFile(dbPath + "-wal");
  removeFile(dbPath + "-shm");
  // CI 可见性：本测试无 SKIP 路径，仍按约定输出（缺失即 CI 无法统计）
  std::printf("[SKIP-COUNT] 0\n");
  std::printf("\n%s (%d failures)\n", g_fail == 0 ? "ALL PASS" : "FAILED", g_fail);
  return g_fail == 0 ? 0 : 1;
}
