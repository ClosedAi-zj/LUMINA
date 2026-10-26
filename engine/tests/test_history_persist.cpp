// M2 T2.4 · history 落盘自测（N26：H1 跨会话 20 步 / H2 删表参数不变 / H4 强杀无半写 / 折叠）
//
// 本测试的核心命题：**history 是可丢弃派生日志，sidecar 是参数唯一真相源。**
// 每条断言都在问两类问题：
//   ① 崩溃/重启后历史能否原样恢复（H1：20 步逐级 undo，每步 max|Δ|=0）；
//   ② 历史坏了/没了，用户看到的参数会不会变（答案必须是「不变」，且显式提示而非静默）。
//
// 覆盖：
//   M0  纯函数：diffOps/applyOps 往返（param / bulk / 数组内层路径 / 无差异 null / 未知 op）
//   M1  迁移 004 幂等（db_version=4，二次迁移 0 变更）
//   H1  20 次提交 → 「重启」→ historyLength=21 → 逐级 undo 20 步每步参数逐字相等
//   H1b undo 游标本身跨会话（undo 2 步后重启，从第 1 步继续）
//   F   每 50 步折叠：base_json=params@50、更早 diff 删除、折叠后重启恢复 + 可 undo 5 步
//   H2  删 history_* 表 → 重建迁移 → 参数不变（派生不是真相源）
//   H4  事务不变量（entries==current_seq）+ 崩溃语义（sidecar 落后于历史 → 参数以
//       sidecar 为准，历史显式重置不静默）+ redo 分支丢弃（undo 后再提交不撞 UNIQUE）
#include <cstdio>
#include <string>
#include <vector>

#include "core/config.h"
#include "core/str_util.h"
#include "core/thread_pool.h"
#include "db/database.h"
#include "db/develop_settings_dao.h"
#include "db/history_dao.h"
#include "db/migrations.h"
#include "db/photo_dao.h"
#include "edit/edit_session.h"
#include "edit/history.h"
#include "json/json_value.h"
#include "rpc/dispatcher.h"
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

void info(const std::string& text) { printf("  [INFO] %s\n", text.c_str()); }

const char* kHome = "D:/LUMINA/build/runtime/test-history-persist/home";
const char* kPhotoDir = "D:/LUMINA/build/runtime/test-history-persist/photos";

std::string canon(const json::JsonValue& v) { return db::DevelopSettingsDao::canonicalJson(v); }

}  // namespace

int main() {
  printf("=== history 落盘自测（T2.4 / H1·H2·H4）===\n");

  makeDirs(kPhotoDir);
  makeDirs(kHome);
  // 清理上次残留（config.dbPath() = <home>/lumina.db）
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
  db::HistoryDao hist(&database);
  core::ThreadPool pool(2);
  xmp::XmpService xmp(&config, &photos, &database, &pool);

  // 单值查询（prepareCached 每次新建语句，key 仅作标记）
  int querySeq = 0;
  auto scalar = [&](const std::string& sql) -> int64_t {
    std::string e;
    auto stmt = database.prepareCached("t24q" + std::to_string(++querySeq), sql, &e);
    if (!stmt || !stmt->valid()) return -1;
    int64_t v = -1;
    if (stmt->step(nullptr)) v = stmt->columnInt64(0);
    stmt->reset();
    return v;
  };

  // ---------------- M1 迁移 004 幂等 ----------------
  printf("\n[M1] 迁移 004 幂等与版本推进\n");
  check(database.schemaVersion() == db::kTargetDbVersion,
        "db_version = kTargetDbVersion = " + std::to_string(db::kTargetDbVersion));
  {
    db::MigrationReport rep;
    int code = db::kMigrationOk;
    check(db::runVersionedMigrations(&database, &code, &rep), "二次 runVersionedMigrations 成功");
    check(rep.upToDate && rep.statementsRun == 0,
          "二次迁移 0 变更（upToDate=" + std::string(rep.upToDate ? "true" : "false") + "）");
    std::string availErr;
    check(hist.available(&availErr), "history 表存在（" + availErr + "）");
  }

  // ---------------- M0 纯函数 ----------------
  printf("\n[M0] diffOps/applyOps 纯函数往返\n");
  {
    json::JsonValue m1 = json::JsonValue::objectValue();
    m1.setItem("id", "m1");
    m1.setItem("exposure", 0.0);
    json::JsonValue masksA = json::JsonValue::arrayValue();
    masksA.addItem(m1);
    json::JsonValue a = xmp::XmpMapper::defaultParams();
    a.setItem("exposure", 0.1);
    a.setItem("masks", masksA);

    // 多处变更 → bulk
    json::JsonValue b = a;
    b.setItem("exposure", 0.15);
    json::JsonValue m1b = m1;
    m1b.setItem("exposure", 0.4);
    json::JsonValue m2 = json::JsonValue::objectValue();
    m2.setItem("id", "m2");
    m2.setItem("type", "radial");
    json::JsonValue masksB = json::JsonValue::arrayValue();
    masksB.addItem(m1b);
    masksB.addItem(m2);
    b.setItem("masks", masksB);
    std::string label;
    json::JsonValue op = edit::diffOps(a, b, &label);
    check(op.get("t").asString() == "bulk" && op.get("ops").size() == 2,
          "多处变更打包 bulk（2 ops，实际 t=" + op.get("t").asString() + "）");
    check(!label.empty(), "bulk label 非空：" + label);
    json::JsonValue arr = json::JsonValue::arrayValue();
    arr.addItem(op);
    json::JsonValue applied;
    check(edit::applyOps(a, arr, &applied) && canon(applied) == canon(b),
          "bulk diff→apply 往返逐字相等");

    // 同长数组内层变化 → masks[0].exposure（架构 op 编码示例的路径形态）
    json::JsonValue c = a;
    json::JsonValue masksC = json::JsonValue::arrayValue();
    masksC.addItem(m1b);
    c.setItem("masks", masksC);
    std::string label2;
    json::JsonValue op2 = edit::diffOps(a, c, &label2);
    check(op2.get("t").asString() == "param" &&
              op2.get("path").asString() == "masks[0].exposure",
          "同长数组下钻：path=masks[0].exposure（实际 " + op2.get("path").asString() + "）");
    check(!label2.empty() && label2.find("masks[0].exposure") != std::string::npos,
          "param label 含路径：" + label2);
    json::JsonValue arr2 = json::JsonValue::arrayValue();
    arr2.addItem(op2);
    json::JsonValue applied2;
    check(edit::applyOps(a, arr2, &applied2) && canon(applied2) == canon(c),
          "数组内层 diff→apply 往返逐字相等");

    // 无差异 → null op（不入历史）
    check(edit::diffOps(a, a, nullptr).isNull(), "无差异 → null op");

    // 未知 op 类型显式失败（零静默）
    json::JsonValue bogus = json::JsonValue::objectValue();
    bogus.setItem("t", "mask");
    json::JsonValue bogusArr = json::JsonValue::arrayValue();
    bogusArr.addItem(bogus);
    json::JsonValue out3;
    check(!edit::applyOps(a, bogusArr, &out3), "未知 op 类型 applyOps 显式失败");
  }

  // ---------------- 夹具：6 张照片（默认参数已写 sidecar）----------------
  printf("\n[夹具] 6 张照片 + 默认参数 sidecar\n");
  const int64_t rootId = photos.ensureRoot(kPhotoDir, "test", &err);
  const int64_t folderId = photos.ensureFolder(rootId, "2026", "2026", &err);
  check(rootId > 0 && folderId > 0, "root/folder 就绪");
  int64_t p1 = 0, p2 = 0, p3 = 0, p4 = 0, p5 = 0, p6 = 0;
  auto makePhoto = [&](const std::string& name, int64_t* outId) {
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
    std::string e;
    if (!photos.upsert(p, outId, &e)) {
      check(false, "upsert " + name + ": " + e);
      return;
    }
    std::string se;
    if (!xmp.saveParams(*outId, xmp::XmpMapper::defaultParams(), &se, /*force=*/true)) {
      check(false, "saveParams " + name + ": " + se);
    }
  };
  makePhoto("H1_0001.RW2", &p1);
  makePhoto("H1_0002.RW2", &p2);
  makePhoto("H1_0003.RW2", &p3);
  makePhoto("H1_0004.RW2", &p4);
  makePhoto("H1_0005.RW2", &p5);
  makePhoto("H1_0006.RW2", &p6);
  check(p1 > 0 && p2 > 0 && p3 > 0 && p4 > 0 && p5 > 0 && p6 > 0, "6 张夹具照片就绪");

  // 会话操作助手
  auto beginSession = [&](edit::EditSessionManager& m, int64_t pid, int64_t* sid,
                          std::string* paramsCanon, bool* restored, int* historyLength,
                          std::string* warnings) -> bool {
    rpc::RpcError e;
    json::JsonValue out = m.begin(pid, &e);
    if (out.isNull()) return false;
    if (sid) *sid = out.get("sessionId").asInt();
    if (paramsCanon) *paramsCanon = canon(out.get("params"));
    if (restored) *restored = out.get("historyRestored").asBool();
    if (historyLength) *historyLength = static_cast<int>(out.get("historyLength").asInt());
    if (warnings) {
      std::string all;
      for (const auto& item : out.get("warnings").arrayItems()) {
        if (!all.empty()) all += " | ";
        all += item.asString();
      }
      *warnings = all;
    }
    return true;
  };
  auto commitParam = [&](edit::EditSessionManager& m, int64_t sid, const std::string& key,
                         double v, std::string* outCanon) -> bool {
    json::JsonValue patch = json::JsonValue::objectValue();
    patch.setItem(key, v);
    json::JsonValue body = json::JsonValue::objectValue();
    body.setItem("patch", patch);
    rpc::RpcError e;
    json::JsonValue out = m.setParams(sid, body, /*commitHistory=*/true, &e);
    if (out.isNull()) return false;
    if (outCanon) *outCanon = canon(out.get("params"));
    return true;
  };

  // ---------------- H1：跨会话 20 步逐级 undo ----------------
  printf("\n[H1] 20 次提交 → 重启 → 逐级 undo 20 步（max|Δ|=0）\n");
  std::string p0Canon;
  std::vector<std::string> expected1;
  {
    edit::EditSessionManager mA(&xmp, &photos);
    int64_t sid = 0;
    bool restored = true;
    int hlen = 0;
    std::string warn;
    check(beginSession(mA, p1, &sid, &p0Canon, &restored, &hlen, &warn) && hlen == 1 &&
              !restored && warn.empty(),
          "H1 首次打开：historyLength=1，无恢复无警告");
    for (int i = 0; i < 20; ++i) {
      std::string c;
      if (!commitParam(mA, sid, "exposure", 0.05 * (i + 1), &c)) {
        check(false, "H1 commit #" + std::to_string(i + 1) + " 失败");
        break;
      }
      expected1.push_back(c);
      if (i == 0 || i == 9 || i == 19) {
        db::HistoryStateRow st;
        std::string se;
        hist.state(p1, &st, &se);
        check(st.exists && st.currentSeq == i + 1,
              "H4 不变量：current_seq=" + std::to_string(st.currentSeq) + "（期望 " +
                  std::to_string(i + 1) + "）");
        check(scalar("SELECT COUNT(*) FROM history_entries WHERE photo_id=" +
                     std::to_string(p1) + ";") == i + 1,
              "H4 不变量：entries 行数 == current_seq（append 单事务无半写）");
      }
    }
    rpc::RpcError e;
    check(!mA.end(sid, /*flush=*/true, &e).isNull(), "H1 end(flush=true) 落盘 sidecar");
  }  // mA 销毁 = 正常退出
  {
    // 「重启」：全新 EditSessionManager，同一库与 sidecar
    edit::EditSessionManager mB(&xmp, &photos);
    int64_t sid = 0;
    bool restored = false;
    int hlen = 0;
    std::string warn;
    check(beginSession(mB, p1, &sid, nullptr, &restored, &hlen, &warn) && restored &&
              hlen == 21 && warn.empty(),
          "H1 重启恢复：historyLength=" + std::to_string(hlen) + "、historyRestored=" +
              std::string(restored ? "true" : "false") + "（期望 21/true）");
    rpc::RpcError e;
    json::JsonValue cur = mB.undo(sid, &e);
    bool allMatch = true;
    if (cur.isNull() || canon(cur.get("params")) != expected1[18]) {
      allMatch = false;
      check(false, "H1 undo #1 != 提交 #19 的参数（期望逐字相等）");
    } else {
      check(true, "H1 undo #1 == 提交 #19 的参数（逐字相等）");
    }
    for (int k = 2; k <= 20; ++k) {
      cur = mB.undo(sid, &e);
      if (cur.isNull()) {
        allMatch = false;
        check(false, "H1 undo #" + std::to_string(k) + " 意外失败");
        break;
      }
      const std::string want = (k == 20) ? p0Canon : expected1[19 - k];
      if (canon(cur.get("params")) != want) {
        allMatch = false;
        check(false, "H1 undo #" + std::to_string(k) + " 参数偏差（max|Δ|!=0）");
        break;
      }
    }
    check(allMatch && canon(cur.get("params")) == p0Canon,
          "H1 undo 20 步后回到初始参数（max|Δ|=0 全程）");
    rpc::RpcError e2;
    json::JsonValue over = mB.undo(sid, &e2);
    check(over.isNull(), "第 21 步 undo 显式失败（nothing to undo，不静默）");
    check(!mB.end(sid, /*flush=*/true, &e2).isNull(), "H1 会话收尾 flush");
  }

  // ---------------- H1b：undo 游标本身跨会话 ----------------
  printf("\n[H1b] undo 2 步后重启 → 从第 1 步继续\n");
  {
    std::vector<std::string> expected5;
    std::string canon5_0;
    {
      edit::EditSessionManager m(&xmp, &photos);
      int64_t sid = 0;
      std::string pc;
      bool restored = true;
      int hlen = 0;
      check(beginSession(m, p5, &sid, &pc, &restored, &hlen, nullptr) && hlen == 1,
            "H1b 首次打开 p5");
      canon5_0 = pc;
      for (int i = 0; i < 3; ++i) {
        std::string c;
        commitParam(m, sid, "contrast", 1.0 * (i + 1), &c);
        expected5.push_back(c);
      }
      rpc::RpcError e;
      check(!m.undo(sid, &e).isNull() && !m.undo(sid, &e).isNull(), "H1b undo 2 步");
      db::HistoryStateRow st;
      std::string se;
      hist.state(p5, &st, &se);
      check(st.exists && st.currentSeq == 1, "H1b 持久化游标 = 1（undo 后写回）");
      check(!m.end(sid, /*flush=*/true, &e).isNull(), "H1b flush（sidecar=第 1 步参数）");
    }
    {
      edit::EditSessionManager m2(&xmp, &photos);
      int64_t sid = 0;
      bool restored = false;
      int hlen = 0;
      std::string pc;
      check(beginSession(m2, p5, &sid, &pc, &restored, &hlen, nullptr) && restored &&
                hlen == 2 && pc == expected5[0],
            "H1b 重启：从第 1 步恢复（historyLength=2，参数==提交#1）");
      rpc::RpcError e;
      json::JsonValue cur = m2.undo(sid, &e);
      check(!cur.isNull() && canon(cur.get("params")) == canon5_0,
            "H1b 继续 undo → 回到初始参数");
      m2.end(sid, /*flush=*/false, &e);
    }
  }

  // ---------------- F：每 50 步折叠 ----------------
  printf("\n[F] 55 次提交：50 步折叠 + 折叠后重启恢复\n");
  {
    std::vector<std::string> expected2;
    {
      edit::EditSessionManager m(&xmp, &photos);
      int64_t sid = 0;
      check(beginSession(m, p2, &sid, nullptr, nullptr, nullptr, nullptr), "F 打开 p2");
      for (int i = 0; i < 55; ++i) {
        std::string c;
        if (!commitParam(m, sid, "exposure", 0.01 * (i + 1), &c)) {
          check(false, "F commit #" + std::to_string(i + 1) + " 失败");
          break;
        }
        expected2.push_back(c);
        if (i == 49) {
          db::HistoryStateRow st;
          std::string se;
          hist.state(p2, &st, &se);
          check(st.exists && st.currentSeq == 50, "F 折叠触发：current_seq=50");
          json::JsonValue base;
          std::string pe;
          check(json::JsonValue::parse(st.baseJson, &base, &pe) &&
                    canon(base) == expected2[49],
                "F base_json == params@50（checkpoint 完整参数）");
          check(st.baseHash == db::HistoryDao::hashOf(base), "F base_hash 与 base_json 一致");
          check(scalar("SELECT COUNT(*) FROM history_entries WHERE photo_id=" +
                       std::to_string(p2) + ";") == 0,
                "F 更早 diff 已删（entries=0，单事务无半写）");
        }
      }
      db::HistoryStateRow st;
      hist.state(p2, &st, &err);
      check(st.currentSeq == 55 &&
                scalar("SELECT COUNT(*) FROM history_entries WHERE photo_id=" +
                       std::to_string(p2) + ";") == 5,
            "F 折叠后继续追加：current_seq=55、diff=5");
      rpc::RpcError e;
      check(!m.end(sid, /*flush=*/true, &e).isNull(), "F flush");
    }
    {
      edit::EditSessionManager m2(&xmp, &photos);
      int64_t sid = 0;
      bool restored = false;
      int hlen = 0;
      std::string pc;
      check(beginSession(m2, p2, &sid, &pc, &restored, &hlen, nullptr) && restored &&
                hlen == 56 && pc == expected2[54],
            "F 折叠后重启：base+5 diff 重放（historyLength=56，参数==提交#55）");
      rpc::RpcError e;
      bool ok = true;
      for (int k = 1; k <= 5; ++k) {
        json::JsonValue cur = m2.undo(sid, &e);
        if (cur.isNull() || canon(cur.get("params")) != expected2[54 - k]) {
          ok = false;
          break;
        }
      }
      check(ok, "F 折叠点后 5 步逐级 undo（含回到折叠点 params@50）");
      json::JsonValue over = m2.undo(sid, &e);
      check(over.isNull(), "F 折叠点之前不可 undo（虚拟灰字步，显式失败不静默）");
      m2.end(sid, /*flush=*/false, &e);
    }
  }

  // ---------------- H2：删表后参数不变 ----------------
  printf("\n[H2] 删 history_* 表 → 重建迁移 → 参数不变\n");
  {
    std::string expected3;
    {
      edit::EditSessionManager m(&xmp, &photos);
      int64_t sid = 0;
      std::string pc;
      check(beginSession(m, p3, &sid, &pc, nullptr, nullptr, nullptr), "H2 打开 p3");
      std::string c;
      for (int i = 0; i < 3; ++i) commitParam(m, sid, "contrast", 1.0 * (i + 1), &c);
      expected3 = c;
      rpc::RpcError e;
      check(!m.end(sid, /*flush=*/true, &e).isNull(), "H2 flush");
    }
    std::string dropErr;
    check(database.exec("DROP TABLE history_entries;", &dropErr) &&
              database.exec("DROP TABLE history_state;", &dropErr),
          "H2 删表成功");
    check(db::writeDbVersion(&database, db::kDbVersionDevelopSettings, nullptr),
          "H2 db_version 回退到 3");
    db::MigrationReport rep;
    int code = db::kMigrationOk;
    check(db::runVersionedMigrations(&database, &code, &rep) && rep.histTableCreated == 1,
          "H2 004 重建 history 表");
    check(db::readDbVersion(&database) == db::kTargetDbVersion, "H2 db_version 回到 4");
    {
      edit::EditSessionManager m2(&xmp, &photos);
      int64_t sid = 0;
      bool restored = true;
      int hlen = 99;
      std::string pc;
      check(beginSession(m2, p3, &sid, &pc, &restored, &hlen, nullptr) && !restored &&
                hlen == 1 && pc == expected3,
            "H2 删表重启：历史为空（historyLength=1）但参数逐字不变（sidecar 是真相源）");
      rpc::RpcError e2;
      m2.end(sid, /*flush=*/false, &e2);
    }
  }

  // ---------------- H4：崩溃语义 + redo 分支丢弃 ----------------
  printf("\n[H4] 崩溃语义（sidecar 落后）+ redo 分支丢弃\n");
  {
    // 场景①：提交后未 flush（防抖窗口内被杀）→ sidecar 落后于持久化历史
    std::string canon4_0;
    {
      edit::EditSessionManager m(&xmp, &photos);
      int64_t sid = 0;
      std::string pc;
      check(beginSession(m, p4, &sid, &pc, nullptr, nullptr, nullptr), "H4 打开 p4");
      canon4_0 = pc;
      std::string c;
      for (int i = 0; i < 3; ++i) commitParam(m, sid, "vibrance", 0.1 * (i + 1), &c);
      // 不 end（模拟崩溃；tick 未跑 → sidecar 仍是初始参数）
      check(scalar("SELECT current_seq FROM history_state WHERE photo_id=" +
                   std::to_string(p4) + ";") == 3,
            "H4 崩溃前：历史已落盘（current_seq=3）");
      // 新「进程」打开同一照片：恢复出的 params@3 与 sidecar params@0 指纹不一致
      edit::EditSessionManager m2(&xmp, &photos);
      int64_t sid2 = 0;
      bool restored = true;
      int hlen = 99;
      std::string pc2;
      std::string warn;
      check(beginSession(m2, p4, &sid2, &pc2, &restored, &hlen, &warn) && !restored &&
                hlen == 1 && pc2 == canon4_0 &&
                warn.find("历史记录与当前参数不一致") != std::string::npos,
            "H4 崩溃后：参数以 sidecar 为准（逐字不变），历史显式重置（零静默）");
      check(scalar("SELECT current_seq FROM history_state WHERE photo_id=" +
                   std::to_string(p4) + ";") == 0,
            "H4 重置后游标归零");
      rpc::RpcError e;
      m.end(sid, /*flush=*/false, &e);  // 旧会话放弃未落盘变更
      m2.end(sid2, /*flush=*/false, &e);
    }
    // 场景②：undo 后再提交 → DB redo 分支被丢弃、seq 不撞 UNIQUE(photo_id,seq)
    {
      edit::EditSessionManager m(&xmp, &photos);
      int64_t sid = 0;
      std::string pc;
      check(beginSession(m, p6, &sid, &pc, nullptr, nullptr, nullptr), "H4② 打开 p6");
      std::string c12, c3;
      commitParam(m, sid, "exposure", 0.1, &c12);
      commitParam(m, sid, "exposure", 0.2, &c12);
      commitParam(m, sid, "exposure", 0.3, &c3);
      rpc::RpcError e;
      json::JsonValue undone = m.undo(sid, &e);
      check(!undone.isNull() && canon(undone.get("params")) == c12, "H4② undo 到提交#2");
      std::string c9;
      check(commitParam(m, sid, "exposure", 0.9, &c9), "H4② undo 后提交新值 0.9");
      check(scalar("SELECT COUNT(*) FROM history_entries WHERE photo_id=" +
                       std::to_string(p6) + ";") == 3 &&
                scalar("SELECT current_seq FROM history_state WHERE photo_id=" +
                       std::to_string(p6) + ";") == 3,
            "H4② redo 分支已随提交丢弃（entries=3、seq 无 UNIQUE 撞号）");
      check(!m.end(sid, /*flush=*/true, &e).isNull(), "H4② flush");
      // 重启验证新分支完整恢复
      edit::EditSessionManager m2(&xmp, &photos);
      int64_t sid2 = 0;
      bool restored = false;
      int hlen = 0;
      std::string pc2;
      check(beginSession(m2, p6, &sid2, &pc2, &restored, &hlen, nullptr) && restored &&
                hlen == 4 && pc2 == c9,
            "H4② 重启恢复新分支（historyLength=4，参数==0.9 提交）");
      m2.end(sid2, /*flush=*/false, &e);
    }
  }

  printf("\n=== 结果：%s（失败 %d 项）===\n", gFailures == 0 ? "全部通过" : "存在失败", gFailures);
  printf("[SKIP-COUNT] 0\n");
  return gFailures == 0 ? 0 : 1;
}
