// v1.2.3 · E1 回归：编辑参数必须「周期落盘」，崩溃/被杀不丢整段编辑
//
// 缺陷事实（修复前）：
//   EditSessionManager::tick() 全仓无调用点，flushThread_ 只声明未使用；
//   setParams() 只把 session->dirty 置 true，sidecar 仅在 flushSession()（被
//   end()/shutdown() 调用）里写。于是「拖了半小时的滑块 + 引擎崩溃」= 参数全丢。
//
// 本测试锁定三条：
//   T1 tick() 路径：begin → setParams（commit=true 一次、commit=false 一次）→ 不调
//      endSession → 等过 500ms 防抖窗口 → 调 tick() → sidecar 已存在且含刚设的值。
//   T2 落盘线程路径：startFlushThread() 后完全不动手调 tick()，只等约 1.2s，
//      sidecar 自动落盘（证明周期线程真的在跑，而不是只有测试手动调用才生效）。
//   T3 develop_settings 派生缓存同步更新（tick 走的是同一个 flushSession，含 upsert）。
//   T4 真并发（E21 重写）：三条子断言
//      T4a 落盘线程在跑，连续 60 次 setParams（每 20ms 一次）期间**一次都不手动 tick()**，
//          停手后 sidecar 必须等于第 60 次的值（期间不许落盘，收尾必须落盘）。
//      T4b 写盘窗口：另起线程调 tick()，在 flushSession 取完快照、sidecar 刚写完、
//          收尾判定还没执行的那一瞬间插入最后一次变更 —— 收尾必须看到它（序号前进），
//          保持 dirty 并在下一个周期落盘。这是原缺陷「清 dirty 丢最后一次拖动」的
//          唯一可复现路径：500ms 防抖门在连续变更期间一直是关的，光靠连发碰不到窗口。
//      T4c 端到端（Esc/切图路径）：连发后立刻 end(flush=true) → flushed=true 且
//          sidecar 立刻等于最后值。
//
// E24b（只改「等」的写法，判据一条没动）：
//   T4a/T4b 原来是墙钟预算（固定 sleep + 读一次），慢机器 / 杀软首次扫描新 exe 时
//   「落盘变慢」会被误报成「变更丢失」。现在改为「轮询到出现为止 + 上限」，
//   并把实际耗时打进日志（轮询会把"变慢"变成静默通过，耗时日志是唯一补救）。
//   夹具清理改为校验 removeFile 返回值 —— 残留 sidecar 删不掉会静默污染下一轮。
//
// 注意：T4a 全程不调 endSession()，这正是缺陷的要害 —— 崩溃时没有 end()。
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#include "core/config.h"
#include "core/str_util.h"
#include "core/thread_pool.h"
#include "db/database.h"
#include "db/photo_dao.h"
#include "edit/edit_session.h"
#include "json/json_value.h"
#include "rpc/dispatcher.h"
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

const char* kHome = "D:/LUMINA/build/runtime/test-edit-flush/home";
const char* kPhotoDir = "D:/LUMINA/build/runtime/test-edit-flush/photos";

void sleepMs(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

// 单值查询（prepareCached 每次新建语句，key 仅作错误定位）
int64_t scalar(db::Database* db, const std::string& sql, int* seq) {
  std::string e;
  auto stmt = db->prepareCached("ef" + std::to_string(++(*seq)), sql, &e);
  if (!stmt || !stmt->valid()) return -1;
  int64_t v = -1;
  if (stmt->step(nullptr)) v = stmt->columnInt64(0);
  stmt->reset();
  return v;
}

// 夹具清理：removeFile() 内部就是 DeleteFileW()!=0，文件本来不存在时也返回 false，
// 所以调用方必须自己区分「本来就没有（正常）」和「删不掉（真失败）」。
// 之前裸调 removeFile() 不作校验：上一轮残留的 sidecar 删不掉时无人知晓，
// 直接把「初始无 sidecar」这类断言变成假的红（实测 run2 的 4 项红全部由它引起）。
void purgeFixtureFile(const std::string& path) {
  if (!fileExists(path)) return;  // 本来就没有：正常
  const bool removed = removeFile(path);
  if (removed && !fileExists(path)) return;  // 删掉且复查确认
  printf("  [FAIL] 夹具清理失败：残留文件删不掉（removeFile 返回 %s）：%s\n",
         removed ? "true" : "false", path.c_str());
  ++gFailures;
}

std::string scalarText(db::Database* db, const std::string& sql, int* seq) {
  std::string e;
  auto stmt = db->prepareCached("eft" + std::to_string(++(*seq)), sql, &e);
  if (!stmt || !stmt->valid()) return std::string();
  std::string v;
  if (stmt->step(nullptr)) v = stmt->columnText(0);
  stmt->reset();
  return v;
}

}  // namespace

int main() {
  printf("=== E1 · 编辑参数周期落盘回归 ===\n");

  makeDirs(kPhotoDir);
  makeDirs(kHome);
  purgeFixtureFile(std::string(kHome) + "/lumina.db");
  purgeFixtureFile(std::string(kHome) + "/lumina.db-wal");
  purgeFixtureFile(std::string(kHome) + "/lumina.db-shm");
  // 清掉上一轮遗留的 sidecar：本测试断言「初始无 sidecar → tick 后才有」，
  // 残留文件会让该断言失去意义（且新库无 sidecar_mtime，会被外部修改检测拦住）。
  purgeFixtureFile(std::string(kPhotoDir) + "/flush_a.xmp");
  purgeFixtureFile(std::string(kPhotoDir) + "/flush_b.xmp");
  purgeFixtureFile(std::string(kPhotoDir) + "/flush_a.RW2");
  purgeFixtureFile(std::string(kPhotoDir) + "/flush_b.RW2");
  purgeFixtureFile(std::string(kPhotoDir) + "/flush_c.xmp");
  purgeFixtureFile(std::string(kPhotoDir) + "/flush_c.RW2");

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

  // ---- 夹具：一张照片（无 sidecar，参数为默认值）----
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
    int64_t id = 0;
    std::string e;
    if (!photos.upsert(p, &id, &e)) {
      check(false, "upsert " + name + ": " + e);
      return static_cast<int64_t>(0);
    }
    return id;
  };

  const int64_t photoA = makePhoto("flush_a.RW2");
  const int64_t photoB = makePhoto("flush_b.RW2");
  const int64_t photoC = makePhoto("flush_c.RW2");
  check(photoA > 0 && photoB > 0 && photoC > 0, "三张照片就绪");

  auto sidecarOf = [&](int64_t photoId) -> std::string {
    PhotoRecord rec;
    std::string e;
    if (!photos.getById(photoId, &rec, &e)) return std::string();
    return rec.sidecarPath.empty() ? xmp.sidecarPathFor(rec.filePath) : rec.sidecarPath;
  };
  auto paramsOf = [&](int64_t photoId, double* exposure, int64_t* contrast) -> bool {
    json::JsonValue out;
    std::string e;
    if (!xmp.loadParams(photoId, &out, &e)) return false;
    *exposure = out.get("exposure").asDouble(-999.0);
    *contrast = out.get("contrast").asInt(-999);
    return true;
  };
  auto putContrast = [](edit::EditSessionManager* mgr, int64_t sid, int64_t value,
                        std::string* err) -> bool {
    json::JsonValue box = json::JsonValue::objectValue();
    json::JsonValue patch = json::JsonValue::objectValue();
    patch.setItem("contrast", value);
    box.setItem("patch", patch);
    rpc::RpcError e;
    const bool ok = !mgr->setParams(sid, box, /*commitHistory=*/false, &e).isNull();
    if (err) *err = e.message;
    return ok;
  };

  // ---------------- T1：tick() 路径（不调 endSession） ----------------
  printf("\n[T1] begin → setParams(commit=true/false) → tick() → sidecar 已落盘\n");
  {
    edit::EditSessionManager sessions(&xmp, &photos);
    rpc::RpcError e;
    json::JsonValue began = sessions.begin(photoA, &e);
    const int64_t sid = began.get("sessionId").asInt(-1);
    check(sid > 0, "begin 返回 sessionId=" + std::to_string(sid) + "（err=" + e.message + "）");

    const std::string sidecar = sidecarOf(photoA);
    check(!sidecar.empty() && !fileExists(sidecar), "初始无 sidecar（" + sidecar + "）");

    json::JsonValue p1 = json::JsonValue::objectValue();
    json::JsonValue patch1 = json::JsonValue::objectValue();
    patch1.setItem("exposure", 0.42);
    p1.setItem("patch", patch1);
    json::JsonValue r1 = sessions.setParams(sid, p1, /*commitHistory=*/true, &e);
    check(!r1.isNull(), "setParams(commit=true) 成功" + (e.message.empty() ? "" : ": " + e.message));

    json::JsonValue p2 = json::JsonValue::objectValue();
    json::JsonValue patch2 = json::JsonValue::objectValue();
    patch2.setItem("contrast", static_cast<int64_t>(17));
    p2.setItem("patch", patch2);
    json::JsonValue r2 = sessions.setParams(sid, p2, /*commitHistory=*/false, &e);
    check(!r2.isNull(), "setParams(commit=false) 成功" + (e.message.empty() ? "" : ": " + e.message));

    // 防抖窗口内必须还没写（证明 tick 是唯一的落盘时机，不是 setParams 顺手写的）
    check(!fileExists(sidecar), "防抖窗口内未落盘（dirty 尚未到 500ms）");

    sleepMs(620);  // 越过 500ms 防抖窗口
    sessions.tick();

    check(fileExists(sidecar), "tick() 后 sidecar 已存在：" + sidecar);
    double exposure = -999.0;
    int64_t contrast = -999;
    check(paramsOf(photoA, &exposure, &contrast), "回读 sidecar 参数成功");
    check(exposure > 0.419 && exposure < 0.421,
          "sidecar 含 exposure=0.42（实际 " + std::to_string(exposure) + "）");
    check(contrast == 17, "sidecar 含 contrast=17（实际 " + std::to_string(contrast) + "）");

    // T3：develop_settings 派生缓存也被 tick 的 flushSession 更新
    const std::string cached =
        scalarText(&database,
                   "SELECT params_json FROM develop_settings WHERE photo_id=" +
                       std::to_string(photoA) + ";",
                   &qseq);
    check(!cached.empty(), "develop_settings 有缓存行");
    check(cached.find("0.42") != std::string::npos,
          "develop_settings 缓存含 exposure=0.42（" + cached.substr(0, 96) + "…）");

    // 不调用 end()，直接析构：证明「没有 end 也没有丢」
  }
  {
    double exposure = -999.0;
    int64_t contrast = -999;
    check(paramsOf(photoA, &exposure, &contrast) && exposure > 0.419 && contrast == 17,
          "会话析构后 sidecar 参数仍在（未 endSession 也不丢）");
  }

  // ---------------- T2：落盘线程路径（不手动 tick） ----------------
  printf("\n[T2] startFlushThread() → setParams → 等 1.2s → 自动落盘\n");
  {
    edit::EditSessionManager sessions(&xmp, &photos);
    sessions.startFlushThread();  // 生产路径：main 装配后启动
    rpc::RpcError e;
    json::JsonValue began = sessions.begin(photoB, &e);
    const int64_t sid = began.get("sessionId").asInt(-1);
    check(sid > 0, "begin 返回 sessionId=" + std::to_string(sid));

    const std::string sidecar = sidecarOf(photoB);
    check(!fileExists(sidecar), "photoB 初始无 sidecar");

    json::JsonValue p = json::JsonValue::objectValue();
    json::JsonValue patch = json::JsonValue::objectValue();
    patch.setItem("contrast", static_cast<int64_t>(33));
    p.setItem("patch", patch);
    check(!sessions.setParams(sid, p, true, &e).isNull(), "setParams 成功");

    // 只等，不调用 tick()：如果落盘线程没跑起来，这里必然失败
    sleepMs(1200);
    check(fileExists(sidecar), "落盘线程自动写入 sidecar：" + sidecar);
    double exposure = -999.0;
    int64_t contrast = -999;
    check(paramsOf(photoB, &exposure, &contrast), "回读 photoB 参数成功");
    check(contrast == 33, "落盘线程写入 contrast=33（实际 " + std::to_string(contrast) + "）");
    // shutdown()（析构内）必须 join 落盘线程：走到这里没有卡死/崩溃即为通过
  }
  printf("  [INFO] EditSessionManager 析构完成（shutdown 已 join 落盘线程）\n");

  // ---------------- T4：写盘窗口内的新变更不被丢弃（真并发） ----------------
  printf("\n[T4a] 落盘线程 + 连续 60 次 setParams（全程不手动 tick）→ 最后一次不丢\n");
  {
    edit::EditSessionManager sessions(&xmp, &photos);
    sessions.startFlushThread();  // 生产路径
    rpc::RpcError e;
    json::JsonValue began = sessions.begin(photoA, &e);
    const int64_t sid = began.get("sessionId").asInt(-1);
    check(sid > 0, "begin(photoA) 返回 sessionId（err=" + e.message + "）");

    const std::string sidecar = sidecarOf(photoA);
    double preExposure = -999.0;
    int64_t preContrast = -999;
    // 先取值再拼消息：check(expr, msg) 的两个实参求值顺序未指定，
    // 写成 check(paramsOf(..., &preContrast), "...preContrast...") 会打印取值前的 -999。
    const bool preRead = paramsOf(photoA, &preExposure, &preContrast);
    check(preRead, "记录突发变更前的 sidecar 值（contrast=" + std::to_string(preContrast) + "）");

    // 每 20ms 一次，共 60 次 ≈ 1.2s。期间**绝不**调用 tick()/end()。
    int64_t last = 0;
    bool burstOk = true;
    for (int i = 1; i <= 60; ++i) {
      std::string pe;
      if (!putContrast(&sessions, sid, static_cast<int64_t>(i), &pe)) {
        check(false, "burst setParams 失败: " + pe);
        burstOk = false;
        break;
      }
      last = i;
      sleepMs(20);
    }
    check(burstOk && last == 60, "连续变更 60 次完成（实际 " + std::to_string(last) + "）");

    // 防抖门语义：最后一次变更距今始终 <500ms，因此整个连发期间不该有任何落盘
    double midExposure = -999.0;
    int64_t midContrast = -999;
    const bool midRead = paramsOf(photoA, &midExposure, &midContrast);
    check(midRead && midContrast == preContrast,
          "连发期间未落盘（" + sidecar + " 仍是 " + std::to_string(preContrast) + "，实际 " +
              std::to_string(midContrast) + "）");

    // 停手后只等，不调 tick()：落盘线程必须在 500ms 防抖窗口过后自己写。
    // E24b：这里原来是「sleepMs(1500) + 读一次」的墙钟预算。慢机器/杀软首执行
    // 扫描新 exe 时单次落盘会被拖到秒级，于是「落盘慢」被误报成「丢变更」
    // （实测 6 连红里就有这种假红）。改为轮询到出现为止（上限 10s）：
    // 丢了就永远等不到 60，超时后下面的断言照样红，所以没掩盖问题；
    // 但轮询会把「变慢」变成静默通过，因此实际耗时必须打进日志。
    const auto t4aStart = std::chrono::steady_clock::now();
    double exposure = -999.0;
    int64_t contrast = -999;
    bool arrived = false;
    while (std::chrono::steady_clock::now() - t4aStart < std::chrono::milliseconds(10000)) {
      if (paramsOf(photoA, &exposure, &contrast) && contrast == 60) {
        arrived = true;
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    const long long t4aMs = static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                                        std::chrono::steady_clock::now() - t4aStart)
                                                        .count());
    printf("  [INFO] T4a 落盘耗时 %lldms（轮询上限 10000ms，%s）\n", t4aMs,
           arrived ? "已落盘" : "超时未见 60");
    check(paramsOf(photoA, &exposure, &contrast), "回读 photoA 参数成功");
    check(contrast == 60, "落盘内容 = 最后一次变更 60（实际 " + std::to_string(contrast) +
                              "；若 <60 说明最后一次拖动被丢弃）");
    // shutdown()（析构内）必须 join 落盘线程：走到这里没有卡死/崩溃即为通过
  }

  // ---- T4b：把变更精确插进「写盘进行中」的窗口 ----
  // 为什么不能只靠 T4a 的连发：500ms 防抖门要求「距最后一次变更 ≥500ms」，
  // 连续变更时该条件永远不成立，tick() 根本不会进 flushSession —— 连发碰不到窗口。
  // 这里另起一个线程调 tick()，用 sidecar 的 mtime 变化作为「写盘刚完成」的信号：
  // 快照在写盘之前取走，而收尾判定在写盘之后（中间还夹着 getById + 派生缓存 upsert），
  // 因此「mtime 刚变」这一刻插入的变更，必然不被本次写盘覆盖、且必然早于收尾判定。
  // 回归前（收尾无条件 dirty=false）：这次变更被无声抹掉，下一个周期不写 → sidecar 停在旧值。
  printf("\n[T4b] 写盘窗口内插入最后一条变更 → 收尾不得抹掉 dirty\n");
  {
    edit::EditSessionManager sessions(&xmp, &photos);  // 不启落盘线程：窗口只由一个 tick 造成
    rpc::RpcError e;
    json::JsonValue began = sessions.begin(photoA, &e);
    const int64_t sid = began.get("sessionId").asInt(-1);
    check(sid > 0, "begin(photoA) 返回 sessionId");

    const std::string sidecar = sidecarOf(photoA);
    const int kRounds = 8;
    int hitRounds = 0;       // 捕捉到「写盘刚完成」信号的轮次（插入点有落在收尾判定之前的窗口）
    int lostRounds = 0;      // 最后一条变更被丢弃的轮次（应为 0）
    int64_t firstLostValue = -1;
    for (int r = 0; r < kRounds; ++r) {
      const int64_t flushedValue = 200 + r * 2;      // 本轮先落盘的值
      const int64_t windowValue = flushedValue + 1;  // 插进写盘窗口的值

      std::string pe;
      if (!putContrast(&sessions, sid, flushedValue, &pe)) {
        check(false, "round " + std::to_string(r) + " 预置变更失败: " + pe);
        break;
      }
      sleepMs(620);  // 越过 500ms 防抖窗口，让下一次 tick 真的会写盘

      // 探测「写盘刚完成」的预算给到 8s：这个忙等不是断言，只是用来定位插入点。
      // 机器刚跑完一轮链接 / 杀软首次扫描新 exe 时，单次 sidecar 落盘会被拖到秒级，
      // 预算太紧会把「机器慢」误报成「没命中窗口」（首轮实测踩到过：8/8 全部误判）。
      const int64_t mtimeBefore = fileMtimeMs(sidecar);
      std::thread ticker([&sessions]() { sessions.tick(); });
      // 忙等（不用 sleep_for：Windows 上 sub-ms 的 sleep 会被取整到毫秒级甚至更粗）
      bool detected = false;
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(8000);
      while (std::chrono::steady_clock::now() < deadline) {
        if (fileMtimeMs(sidecar) != mtimeBefore) {
          detected = true;
          break;
        }
      }
      if (detected) ++hitRounds;
      const bool inserted = putContrast(&sessions, sid, windowValue, &pe);
      ticker.join();
      if (!inserted) {
        check(false, "round " + std::to_string(r) + " 窗口插入失败: " + pe);
        break;
      }

      // 下一个巡检周期（500ms 防抖 + 一次 tick）必须把窗口内那条变更写下去。
      // E24b：原来是「sleepMs(700) + 一次 tick + 读一次」，是墙钟预算；
      // 慢机器上「下一个周期还没到」会被误报成「变更被丢弃」。改为轮询到出现为止
      // （上限 5s，每 20ms 一次 tick）：变更若真被抹掉，tick 再多次也写不出来，
      // 超时后 lostRounds 照样 +1，所以没有掩盖丢失。耗时打进日志暴露「变慢」。
      const auto t4bStart = std::chrono::steady_clock::now();
      double ex = -999.0;
      int64_t ct = -999;
      bool written = false;
      while (std::chrono::steady_clock::now() - t4bStart < std::chrono::milliseconds(5000)) {
        sessions.tick();  // 巡检周期：防抖门过后才会真的写盘
        if (paramsOf(photoA, &ex, &ct) && ct == windowValue) {
          written = true;
          break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
      }
      const long long t4bMs = static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                                          std::chrono::steady_clock::now() - t4bStart)
                                                          .count());
      printf("  [INFO] T4b 第%d轮耗时 %lldms（%s，捕捉到写盘信号=%s）\n", r, t4bMs,
             written ? "已落盘" : "超时未见窗口值", detected ? "是" : "否");
      if (!written) {
        ++lostRounds;
        if (firstLostValue < 0) firstLostValue = ct;
        printf("  [INFO] round %d 丢失：期望 %lld，盘上 %lld（捕捉到写盘信号=%s）\n", r,
               static_cast<long long>(windowValue), static_cast<long long>(ct),
               detected ? "是" : "否");
      }
    }
    printf("  [INFO] 捕捉到写盘完成信号 %d/%d 轮\n", hitRounds, kRounds);
    check(hitRounds > 0, "至少一轮在写盘期间插入了变更（实际 " + std::to_string(hitRounds) +
                             "/" + std::to_string(kRounds) + "）");
    check(lostRounds == 0, "窗口内插入的变更全部落盘（丢失 " + std::to_string(lostRounds) +
                               " 轮" +
                               (firstLostValue < 0
                                    ? std::string(")")
                                    : "，首个丢失轮盘上值 " + std::to_string(firstLostValue) + ")"));
  }

  // ---- T4c：端到端（Esc/切图路径）：连发后立刻 end(flush=true) ----
  printf("\n[T4c] 连发后立刻 end(flush=true) → flushed=true 且 sidecar 立刻是最后值\n");
  {
    edit::EditSessionManager sessions(&xmp, &photos);
    rpc::RpcError e;
    json::JsonValue began = sessions.begin(photoC, &e);
    const int64_t sid = began.get("sessionId").asInt(-1);
    check(sid > 0, "begin(photoC) 返回 sessionId");

    const std::string sidecar = sidecarOf(photoC);
    check(!fileExists(sidecar), "photoC 初始无 sidecar（" + sidecar + "）");

    int64_t last = 0;
    bool burstOk = true;
    for (int i = 1; i <= 40; ++i) {
      std::string pe;
      if (!putContrast(&sessions, sid, 300 + static_cast<int64_t>(i), &pe)) {
        check(false, "T4c burst setParams 失败: " + pe);
        burstOk = false;
        break;
      }
      last = 300 + i;
      sleepMs(10);
    }
    check(burstOk && last == 340, "T4c 连续变更 40 次完成（实际 " + std::to_string(last) + "）");
    check(!fileExists(sidecar), "T4c 连发期间未落盘（防抖窗口内，且没调 end）");

    json::JsonValue ended = sessions.end(sid, /*flush=*/true, &e);
    check(!ended.isNull(), "T4c end(flush=true) 成功" + (e.message.empty() ? "" : ": " + e.message));
    check(ended.get("flushed").asBool(false), "T4c flushed == true");
    check(!ended.get("sidecarMtime").isNull(), "T4c 返回非空 sidecarMtime");
    double ex = -999.0;
    int64_t ct = -999;
    const bool readBack = paramsOf(photoC, &ex, &ct);
    check(readBack && ct == last, "T4c end 后 sidecar 立刻 = 最后值 " + std::to_string(last) +
                                      "（实际 " + std::to_string(ct) + "）");
  }

  printf("\n=== 结果：%s（失败 %d 项）===\n", gFailures == 0 ? "PASS" : "FAIL", gFailures);
  return gFailures == 0 ? 0 : 1;
}
