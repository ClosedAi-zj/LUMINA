// M2 T2.2b⑥ · 启动回填 job 自测
//
// 直接测 job 的执行函数 runRebuildDevelopSettings（不测 main.cpp 装配，与仓库
// 现有测试风格一致）。覆盖任务书四条 + 低优先级实测打印：
//   B1  首跑判定：develop_settings 为空 → mode="full"，COUNT(*) == has_edits=1 行数
//   B2  二跑判定：无变化 → mode="incremental" 且 0 写（mode 断言即任务书④）
//   B3  增量口径：sidecar mtime +60s 后二跑 scanned/written 只含该行，其余行不重写
//   B4  取消：跑一半 cancel → cancelled=true、结果如实、已提交部分保持有效、不静默
//   B5  零静默：表不可用 → mode="unavailable"（不当作无事发生）；复原后全量重建仍对
//   P   低优先级实测：全量回填总耗时（批间 yield 占比）打印，供回执引用
#include <atomic>
#include <cstdio>
#include <ctime>
#include <string>
#include <vector>

#include <sys/utime.h>

#include "core/config.h"
#include "core/str_util.h"
#include "core/thread_pool.h"
#include "db/database.h"
#include "db/develop_settings_dao.h"
#include "db/migrations.h"
#include "db/photo_dao.h"
#include "jobs/rebuild_develop_settings.h"
#include "json/json_value.h"
#include "xmp/xmp_mapper.h"
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

const char* kHome = "D:/LUMINA/build/runtime/test-backfill/home";
const char* kPhotoDir = "D:/LUMINA/build/runtime/test-backfill/photos";

const int kEdited = 120;  // > 2 批（kBackfillChunk=50），保证取消测试跨批
const int kPlain = 8;     // 未调整、无 sidecar：全量重建后不该有缓存行

// 行快照：只比数据列（source / updated_at 每次回填都会变，不比）
struct RowSnapshot {
  int64_t photoId = 0;
  std::string paramsJson;
  std::string paramsHash;
  int64_t sidecarMtime = 0;
  int maskCount = 0;
  bool hasEdits = false;
};

bool snapshotRow(db::DevelopSettingsDao* dao, int64_t photoId, RowSnapshot* out) {
  db::DevelopSettingsRow row;
  bool found = false;
  std::string err;
  if (!dao->get(photoId, &row, &found, &err)) return false;
  if (!found) return false;
  out->photoId = row.photoId;
  out->paramsJson = row.paramsJson;
  out->paramsHash = row.paramsHash;
  out->sidecarMtime = row.sidecarMtime;
  out->maskCount = row.maskCount;
  out->hasEdits = row.hasEdits;
  return true;
}

bool sameSnapshot(const RowSnapshot& a, const RowSnapshot& b) {
  return a.photoId == b.photoId && a.paramsJson == b.paramsJson && a.paramsHash == b.paramsHash &&
         a.sidecarMtime == b.sidecarMtime && a.maskCount == b.maskCount &&
         a.hasEdits == b.hasEdits;
}

int64_t countRows(db::Database* db, const std::string& where) {
  auto stmt = db->prepareCached("backfill:count", "SELECT COUNT(*) FROM develop_settings " +
                                                      where + ";", nullptr);
  if (!stmt || !stmt->valid() || !stmt->step(nullptr)) return -1;
  const int64_t n = stmt->columnInt64(0);
  stmt->reset();
  return n;
}

// 把 sidecar 的 mtime 拨到未来 +60s（确定性触发增量口径，避免依赖写盘同毫秒）
bool bumpSidecarMtime(const std::string& path) {
  struct _utimbuf times;
  times.actime = ::time(nullptr) + 60;
  times.modtime = times.actime;
  return ::_utime(path.c_str(), &times) == 0;
}

}  // namespace

int main() {
  printf("=== 启动回填 job 自测（M2 T2.2b⑥）===\n");

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
  db::PhotoDao photos(&database);
  db::DevelopSettingsDao dao(&database);
  core::ThreadPool pool(2);
  xmp::XmpService xmp(&config, &photos, &database, &pool);

  // ---------------- 夹具：120 张已调整（真 sidecar）+ 8 张未调整 ----------------
  printf("\n[夹具] %d 张已调整照片（带真 sidecar）+ %d 张未调整\n", kEdited, kPlain);
  std::vector<int64_t> editedIds;
  std::vector<int64_t> plainIds;
  {
    const int64_t rootId = photos.ensureRoot(kPhotoDir, "test", &err);
    const int64_t folderId = photos.ensureFolder(rootId, "2026", "2026", &err);
    check(rootId > 0 && folderId > 0, "root/folder 就绪");
    for (int i = 0; i < kEdited + kPlain; ++i) {
      PhotoRecord p;
      p.fileName = "BF_" + std::to_string(2000 + i) + ".RW2";
      p.filePath = std::string(kPhotoDir) + "/" + p.fileName;
      p.format = "RW2";
      p.rootId = rootId;
      p.folderId = folderId;
      p.capturedAt = 1767225600000LL + i * 1000;
      p.importedAt = nowMs();
      p.width = 64;
      p.height = 48;
      p.health = "ok";
      int64_t id = 0;
      if (!photos.upsert(p, &id, &err)) {
        printf("  [FAIL] upsert %s: %s\n", p.fileName.c_str(), err.c_str());
        ++gFailures;
        continue;
      }
      if (i < kEdited) {
        editedIds.push_back(id);
        json::JsonValue params = xmp::XmpMapper::defaultParams();
        params.setItem("exposure", 0.1 * static_cast<double>(i % 7 + 1));
        std::string saveErr;
        if (!xmp.saveParams(id, params, &saveErr, /*force=*/true)) {
          printf("  [FAIL] saveParams %s: %s\n", p.fileName.c_str(), saveErr.c_str());
          ++gFailures;
        }
      } else {
        plainIds.push_back(id);
      }
    }
  }
  check(static_cast<int>(editedIds.size()) == kEdited, "已调整照片就绪");
  check(static_cast<int>(plainIds.size()) == kPlain, "未调整照片就绪");
  int64_t rows0 = 0;
  check(dao.cachedRowCount(&rows0, &err) && rows0 == 0, "初始 develop_settings 为空（首跑前提）");

  // ---------------- B1 首跑全量判定 ----------------
  printf("\n[B1] 首跑判定：表空 → mode=full，行数与 has_edits=1 对齐\n");
  jobs::RebuildJobResult r1;
  {
    std::atomic<bool> cancel(false);
    std::string runErr;
    r1 = jobs::runRebuildDevelopSettings(&database, &xmp, &cancel, nullptr, &runErr);
    check(r1.mode == "full", "mode = full（实际 " + r1.mode + "）");
    check(r1.failed == 0, "失败 0 行（实际 " + std::to_string(r1.failed) + "）");
    check(r1.cancelled == false, "未取消");
    const int64_t total = countRows(&database, "");
    const int64_t editedRows = countRows(&database, "WHERE has_edits=1");
    check(total == kEdited, "COUNT(*) = " + std::to_string(total) + "（期望 " +
                                std::to_string(kEdited) + "：未调整照片无 sidecar 不写行）");
    check(editedRows == total, "has_edits=1 行数 = " + std::to_string(editedRows) +
                                   "，与 COUNT(*) 一致");
    check(r1.written == kEdited, "written = " + std::to_string(r1.written) + "（期望 " +
                                     std::to_string(kEdited) + "）");
    // [P] 低优先级实测：全量回填总耗时（含批间 20ms yield 与 sidecar 解析+写库）
    printf("  [P] 全量回填 %d 张耗时 %lld ms（含解析+写库+节流）\n", kEdited,
           static_cast<long long>(r1.durationMs));
  }

  // ---------------- B2 二跑增量判定（任务书④：mode 断言） ----------------
  printf("\n[B2] 二跑判定：缓存齐全且 mtime 未变 → mode=incremental，0 写\n");
  {
    std::atomic<bool> cancel(false);
    std::string runErr;
    jobs::RebuildJobResult r2 = jobs::runRebuildDevelopSettings(&database, &xmp, &cancel,
                                                                nullptr, &runErr);
    check(r2.mode == "incremental", "mode = incremental（实际 " + r2.mode + "）");
    check(r2.scanned == 0, "scanned = " + std::to_string(r2.scanned) + "（无候选）");
    check(r2.written == 0, "written = 0（缓存齐全不重写）");
    check(r2.failed == 0, "失败 0 行");
  }

  // ---------------- B3 增量只回填 mtime 变新的行 ----------------
  printf("\n[B3] 增量：一张 sidecar mtime +60s → scanned/written 只含该行\n");
  {
    const int64_t victim = editedIds[3];
    const int64_t neighbor = editedIds[4];
    PhotoRecord rec;
    std::string recErr;
    check(photos.getById(victim, &rec, &recErr), "取 victim 记录");
    const std::string sidecar =
        rec.sidecarPath.empty() ? xmp.sidecarPathFor(rec.filePath) : rec.sidecarPath;
    check(bumpSidecarMtime(sidecar), "victim sidecar mtime +60s");

    RowSnapshot neighborBefore;
    check(snapshotRow(&dao, neighbor, &neighborBefore), "neighbor 快照（回填前）");

    std::atomic<bool> cancel(false);
    std::string runErr;
    jobs::RebuildJobResult r3 = jobs::runRebuildDevelopSettings(&database, &xmp, &cancel,
                                                                nullptr, &runErr);
    check(r3.mode == "incremental", "mode = incremental");
    check(r3.scanned == 1, "scanned = " + std::to_string(r3.scanned) + "（期望 1，只含 victim）");
    check(r3.written == 1, "written = " + std::to_string(r3.written) + "（期望 1）");
    check(r3.failed == 0, "失败 0 行");

    RowSnapshot victimAfter;
    RowSnapshot neighborAfter;
    check(snapshotRow(&dao, victim, &victimAfter), "victim 回填后仍有行");
    check(snapshotRow(&dao, neighbor, &neighborAfter), "neighbor 回填后仍有行");
    const int64_t newMtime = fileMtimeMs(sidecar);
    check(victimAfter.sidecarMtime == newMtime,
          "victim sidecar_mtime 已更新为盘上值（" + std::to_string(victimAfter.sidecarMtime) +
              "）");
    check(sameSnapshot(neighborBefore, neighborAfter), "neighbor 行未被重写（逐列相等）");
    const int64_t total = countRows(&database, "");
    check(total == kEdited, "回填后行数仍 = " + std::to_string(total) + "（无重复行）");
  }

  // ---------------- B4 取消：跑一半 cancel ----------------
  printf("\n[B4] 取消：第 2 批进度回调后 cancel → cancelled=true、结果如实\n");
  {
    // 先让一张落入候选（保证取消发生时有真实工作可被打断）
    const int64_t victim = editedIds[7];
    PhotoRecord rec;
    std::string recErr;
    photos.getById(victim, &rec, &recErr);
    bumpSidecarMtime(rec.sidecarPath.empty() ? xmp.sidecarPathFor(rec.filePath)
                                             : rec.sidecarPath);

    std::atomic<bool> cancel(false);
    int progressCalls = 0;
    auto progress = [&cancel, &progressCalls](int percent, const std::string& detail) {
      ++progressCalls;
      if (progressCalls == 3) cancel.store(true);  // 0% 首报 + 第 1 批后 → 第 2 批前取消
      (void)percent;
      (void)detail;
    };
    std::string runErr;
    jobs::RebuildJobResult r4 = jobs::runRebuildDevelopSettings(&database, &xmp, &cancel,
                                                                progress, &runErr);
    check(r4.cancelled, "cancelled = true（实际 " + std::string(r4.cancelled ? "true" : "false") +
                            "）");
    check(!runErr.empty(), "err 非空（不静默）: " + runErr);
    check(r4.mode == "incremental", "mode 如实 = " + r4.mode);
    const int64_t total = countRows(&database, "");
    check(total == kEdited, "已提交部分保持有效：行数仍 = " + std::to_string(total) + "（期望 " +
                                std::to_string(kEdited) + "）");
  }

  // ---------------- B5 零静默：表不可用 + 复原后仍可全量 ----------------
  printf("\n[B5] 表不可用 → mode=unavailable（零静默）；复原后全量重建仍对\n");
  {
    check(database.exec("DROP TABLE develop_settings;", &err), "删表成功");
    std::atomic<bool> cancel(false);
    std::string runErr;
    jobs::RebuildJobResult r5 = jobs::runRebuildDevelopSettings(&database, &xmp, &cancel,
                                                                nullptr, &runErr);
    check(r5.mode == "unavailable", "mode = unavailable（实际 " + r5.mode + "）");
    check(!runErr.empty(), "err 非空（零静默）: " + runErr);

    // 复原：版本回退 → 版本化迁移重建 003 → 表空 → 全量重建
    check(db::writeDbVersion(&database, db::kDbVersionFilesTable, nullptr), "db_version 回退");
    int code = db::kMigrationOk;
    db::MigrationReport rep;
    check(db::runVersionedMigrations(&database, &code, &rep) && rep.devTableCreated == 1,
          "003 重新建表");
    std::string runErr2;
    jobs::RebuildJobResult r6 = jobs::runRebuildDevelopSettings(&database, &xmp, &cancel,
                                                                nullptr, &runErr2);
    check(r6.mode == "full", "复原后判定回 full（实际 " + r6.mode + "）");
    check(r6.written == kEdited && r6.failed == 0,
          "复原后全量重建写入 " + std::to_string(r6.written) + "（期望 " +
              std::to_string(kEdited) + "），失败 " + std::to_string(r6.failed));
  }

  printf("\n=== 结果：%s（失败 %d 项）===\n", gFailures == 0 ? "全部通过" : "存在失败", gFailures);
  return gFailures == 0 ? 0 : 1;
}
