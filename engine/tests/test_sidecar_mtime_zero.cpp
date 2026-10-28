// v1.2.3 · E16 回归：sidecar_mtime = 0 的语义是「未知」，不能被当成「已知 mtime=0」
//
// 缺陷事实（修复前）：
//   photo_dao.cpp bindRecordTail 用 b(p.sidecarMtime) 把 0 绑成**整数 0**，而读回侧
//   `hasSidecarMtime = !columnIsNull(...)` → 读成「true + mtime 0」；
//   xmp_service.cpp 的外部修改检测是 `hasSidecarMtime && current > 0 && current != p.sidecarMtime`
//   → 磁盘上已有 sidecar（current > 0）时 `current != 0` 恒真 → 每次保存都返回
//   "external conflict"。用户视角：导入一张自带 XMP 的照片，第一次调曝光就存不进去。
//
// 本测试锁定三条：
//   M1 写入侧：sidecar_mtime 传 0 必须落成 SQL NULL（upsert 与 setSidecar 两条路径），
//      读回 hasSidecarMtime=false —— 而不是「true + 0」。
//   M2 读取侧：模拟历史脏数据（DB 里 sidecar_mtime 显式为 0，磁盘上 sidecar 真实存在），
//      saveParams(force=false) 必须**成功**且 sidecar 内容已更新；checkExternal 不得报 changed。
//   M3 红线不许修没：已知 mtime>0 且磁盘被外部改过时，saveParams 仍必须返回
//      false + "external conflict"（绝不覆盖用户手改的 XMP）。
#include <cstdio>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "core/config.h"
#include "core/str_util.h"
#include "core/thread_pool.h"
#include "db/database.h"
#include "db/photo_dao.h"
#include "json/json_value.h"
#include "rpc/dispatcher.h"
#include "xmp/xmp_document.h"
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

const char* kHome = "D:/LUMINA/build/runtime/test-sidecar-mtime/home";
const char* kPhotoDir = "D:/LUMINA/build/runtime/test-sidecar-mtime/photos";

void sleepMs(int ms) {
  // 只用于等 mtime 变化（NTFS ms 级）
  std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

enum class MtimeKind { kNull, kZero, kPositive, kUnknown };

MtimeKind readMtimeKind(db::Database* db, int64_t photoId, int64_t* value, int* seq) {
  std::string e;
  auto stmt = db->prepareCached("sm" + std::to_string(++(*seq)),
                                "SELECT sidecar_mtime FROM photos WHERE id=?;", &e);
  if (!stmt || !stmt->valid()) return MtimeKind::kUnknown;
  stmt->bindInt64(1, photoId);
  MtimeKind kind = MtimeKind::kUnknown;
  if (stmt->step(nullptr)) {
    if (stmt->columnIsNull(0)) {
      kind = MtimeKind::kNull;
    } else {
      const int64_t v = stmt->columnInt64(0);
      *value = v;
      kind = (v == 0) ? MtimeKind::kZero : MtimeKind::kPositive;
    }
  }
  stmt->reset();
  return kind;
}

}  // namespace

int main() {
  printf("=== E16 · sidecar_mtime=0（未知）语义回归 ===\n");

  makeDirs(kPhotoDir);
  makeDirs(kHome);
  removeFile(std::string(kHome) + "/lumina.db");
  removeFile(std::string(kHome) + "/lumina.db-wal");
  removeFile(std::string(kHome) + "/lumina.db-shm");
  removeFile(std::string(kPhotoDir) + "/mtime_a.RW2");
  removeFile(std::string(kPhotoDir) + "/mtime_a.xmp");
  removeFile(std::string(kPhotoDir) + "/mtime_b.RW2");
  removeFile(std::string(kPhotoDir) + "/mtime_b.xmp");

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
  core::ThreadPool pool(2);
  xmp::XmpService xmp(&config, &photos, &database, &pool);

  int qseq = 0;

  const int64_t rootId = photos.ensureRoot(kPhotoDir, "test", &err);
  const int64_t folderId = photos.ensureFolder(rootId, "2026", "2026", &err);
  check(rootId > 0 && folderId > 0, "root/folder 就绪");

  auto makePhoto = [&](const std::string& name) -> int64_t {
    PhotoRecord p;
    p.fileName = name;
    p.filePath = std::string(kPhotoDir) + "/" + name;
    p.format = "RW2";
    p.rootId = rootId;
    p.folderId = folderId;
    p.importedAt = nowMs();
    p.width = 64;
    p.height = 48;
    p.health = "ok";
    // 刻意不设 sidecarMtime（保持 0 = 未知），模拟 importer 的正常导入
    int64_t id = 0;
    std::string e;
    if (!photos.upsert(p, &id, &e)) {
      check(false, "upsert " + name + ": " + e);
      return static_cast<int64_t>(0);
    }
    return id;
  };

  const int64_t photoA = makePhoto("mtime_a.RW2");
  const int64_t photoB = makePhoto("mtime_b.RW2");
  check(photoA > 0 && photoB > 0, "两张照片入库");

  auto sidecarOf = [&](int64_t photoId) -> std::string {
    PhotoRecord rec;
    std::string e;
    if (!photos.getById(photoId, &rec, &e)) return std::string();
    return rec.sidecarPath.empty() ? xmp.sidecarPathFor(rec.filePath) : rec.sidecarPath;
  };

  // ---------------- M1：0 → SQL NULL ----------------
  printf("\n[M1] sidecar_mtime=0 必须落成 NULL（upsert / setSidecar 两条路径）\n");
  {
    int64_t v = -1;
    const MtimeKind kindA = readMtimeKind(&database, photoA, &v, &qseq);
    check(kindA == MtimeKind::kNull,
          "upsert(sidecarMtime=0) → SQL NULL（kind=" + std::to_string(static_cast<int>(kindA)) +
              ", value=" + std::to_string(v) + "）");

    PhotoRecord rec;
    std::string e;
    check(photos.getById(photoA, &rec, &e), "getById 成功");
    check(!rec.hasSidecarMtime,
          "读回 hasSidecarMtime=false（不是「已知 mtime=0」）");

    // setSidecar 传 0 也要落 NULL（time 未知时调用方会传 0）
    check(photos.setSidecar(photoB, sidecarOf(photoB), 0, false, &e),
          "setSidecar(mtime=0) 调用成功（" + e + "）");
    int64_t v2 = -1;
    check(readMtimeKind(&database, photoB, &v2, &qseq) == MtimeKind::kNull,
          "setSidecar(mtime=0) → SQL NULL");
  }

  // ---------------- M2：历史脏数据（DB 里为 0）+ 磁盘有 sidecar → 必须能保存 ----------------
  printf("\n[M2] DB 记录 sidecar_mtime=0（历史脏数据）+ 磁盘有 sidecar → saveParams 成功\n");
  {
    const std::string sidecar = sidecarOf(photoA);

    // 先用 force 写一份合法 sidecar（模拟「导入时照片自带 XMP」）
    json::JsonValue base = xmp::XmpMapper::defaultParams();
    base.setItem("exposure", 0.0);
    std::string saveErr;
    check(xmp.saveParams(photoA, base, &saveErr, /*force=*/true),
          "准备：force 写入 sidecar（" + saveErr + "）");
    check(fileExists(sidecar), "sidecar 已存在于磁盘：" + sidecar);

    // 把 DB 的 sidecar_mtime 手动改成 0 —— 精确复现修复前的脏数据状态
    check(database.exec("UPDATE photos SET sidecar_mtime=0 WHERE id=" + std::to_string(photoA) +
                            ";",
                        &saveErr),
          "构造历史脏数据：sidecar_mtime=0");
    int64_t v = -1;
    check(readMtimeKind(&database, photoA, &v, &qseq) == MtimeKind::kZero,
          "确认 DB 里现在确实是整数 0（修复前会读成「已知 mtime=0」）");

    PhotoRecord rec;
    std::string recErr;
    check(photos.getById(photoA, &rec, &recErr) && rec.hasSidecarMtime && rec.sidecarMtime == 0,
          "读回形态 = hasSidecarMtime=true + mtime=0（正是触发误判的形态）");

    // 这种形态不得被当成外部修改
    rpc::RpcError cErr;
    json::JsonValue ext = xmp.checkExternal(photoA, &cErr);
    check(!ext.isNull() && ext.get("changed").asBool(true) == false,
          "checkExternal: 未知 mtime 不报 changed（err=" + cErr.message + "）");

    // 关键断言：保存必须成功
    json::JsonValue edited = base;
    edited.setItem("exposure", 0.66);
    std::string editErr;
    const bool saved = xmp.saveParams(photoA, edited, &editErr);
    check(saved, "saveParams(force=false) 成功（修复前会返回 external conflict；本次 err=" +
                     editErr + "）");

    // 内容真的写进去了
    json::JsonValue readBack;
    std::string readErr;
    check(xmp.loadParams(photoA, &readBack, &readErr), "回读 sidecar 成功（" + readErr + "）");
    const double exposure = readBack.get("exposure").asDouble(-1.0);
    check(exposure > 0.659 && exposure < 0.661,
          "sidecar 内容已更新 exposure=0.66（实际 " + std::to_string(exposure) + "）");

    // 成功保存后 DB 应被刷新为「已知的正 mtime」
    int64_t after = -1;
    const MtimeKind kindAfter = readMtimeKind(&database, photoA, &after, &qseq);
    check(kindAfter == MtimeKind::kPositive,
          "保存成功后 sidecar_mtime 刷新为正数（实际 " + std::to_string(after) + "）");
  }

  // ---------------- M3：红线——已知 mtime 且被外部改过 → 仍须拒绝 ----------------
  printf("\n[M3] 红线保留：已知 mtime>0 且磁盘被外部改过 → external conflict\n");
  {
    const std::string sidecar = sidecarOf(photoA);
    PhotoRecord rec;
    std::string recErr;
    check(photos.getById(photoA, &rec, &recErr) && rec.hasSidecarMtime && rec.sidecarMtime > 0,
          "前置：DB 中 mtime 已是已知正数");

    // 外部工具改 sidecar：读出 → 改一个字段 → 写回（mtime 必然变化）
    std::string bytes;
    std::string ioErr;
    check(readFile(sidecar, &bytes, &ioErr), "读取 sidecar 供外部修改（" + ioErr + "）");
    std::string parseErr;
    std::unique_ptr<xmp::XmpDocument> doc = xmp::XmpDocument::parse(bytes, &parseErr);
    check(doc != nullptr, "解析 sidecar 成功（" + parseErr + "）");
    if (doc) {
      doc->setText("crs", "Contrast2012", "42");  // 模拟外部工具改了对比度
      sleepMs(30);                                // 让 mtime 明确前进
      check(writeFileBytes(sidecar, doc->serialize(), &ioErr),
            "外部写入 sidecar（mtime 已变化）");
    }

    // checkExternal 必须报 changed
    rpc::RpcError cErr;
    json::JsonValue ext = xmp.checkExternal(photoA, &cErr);
    check(!ext.isNull() && ext.get("changed").asBool(false),
          "checkExternal 报 changed=true（外部修改被识别）");

    // saveParams 必须拒绝覆盖
    json::JsonValue overwrite = xmp::XmpMapper::defaultParams();
    overwrite.setItem("exposure", 3.0);
    std::string owErr;
    const bool saved = xmp.saveParams(photoA, overwrite, &owErr);
    check(!saved, "saveParams 拒绝覆盖外部修改（返回 false）");
    check(owErr.find("external conflict") != std::string::npos,
          "错误原因为 external conflict（实际 " + owErr + "）");

    // 外部内容没被冲掉
    json::JsonValue readBack;
    std::string readErr;
    check(xmp.loadParams(photoA, &readBack, &readErr), "回读 sidecar 成功");
    check(readBack.get("contrast").asInt(-1) == 42,
          "外部写入的 contrast=42 未被覆盖（实际 " +
              std::to_string(readBack.get("contrast").asInt(-1)) + "）");
    check(readBack.get("exposure").asDouble(3.0) < 2.9,
          "引擎的 exposure=3.0 未被写入");
  }

  printf("\n=== 结果：%s（失败 %d 项）===\n", gFailures == 0 ? "PASS" : "FAIL", gFailures);
  return gFailures == 0 ? 0 : 1;
}
