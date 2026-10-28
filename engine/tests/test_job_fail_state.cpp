// v1.2.3 · E8 回归：任务终态不得谎报成功，取消不得恒回 true
//
// 缺陷事实：
//   ① JobContext 只有 progress/setResult/cancelRequested，**没有失败态**；
//      importer.cpp 扫描失败时 setResult(全 0 统计) → JobManager 终态判 done，
//      UI 弹出"导入完成 0 张"，真因（源目录不可访问）彻底丢失。
//   ② methods.cpp 的 import.cancel / health.cancelJob 丢弃 cancel() 返回值固定回
//      {"cancelled": true}；JobManager::cancel 对已 done 的任务也回 true。
//
// 本测试锁定：
//   J1 ctx.fail() → 终态 failed，且 event.job.completed 带 error.code/message
//   J2 ctx.setResult()（正常路径）仍是 done —— 不能因为加了失败态把成功判错
//   J3 cancel(已结束) == false，cancel(不存在) == false，cancel(运行中) == true
//   J4 不可访问的源目录 → import job 终态 failed（不是 done）
#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "core/config.h"
#include "core/job_manager.h"
#include "core/str_util.h"
#include "core/thread_pool.h"
#include "db/database.h"
#include "db/fts_indexer.h"
#include "db/image_dao.h"
#include "db/photo_dao.h"
#include "import/importer.h"
#include "json/json_value.h"
#include "raw/raw_decoder.h"
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

const char* kHome = "D:/LUMINA/build/runtime/test-job-fail/home";
const char* kPhotoDir = "D:/LUMINA/build/runtime/test-job-fail/photos";

void sleepMs(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

// 轮询等待任务进入终态（最多 waitMs 毫秒）
bool waitTerminal(JobManager* jobs, int64_t jobId, JobState* out, int waitMs) {
  const int step = 20;
  for (int waited = 0; waited <= waitMs; waited += step) {
    JobState st;
    if (jobs->getJob(jobId, &st)) {
      if (st.state == "done" || st.state == "failed" || st.state == "cancelled") {
        *out = st;
        return true;
      }
    }
    sleepMs(step);
  }
  return false;
}

}  // namespace

int main() {
  printf("=== E8 · 任务失败态 / 取消返回值 回归 ===\n");

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

  core::ThreadPool pool(2);
  // 收集 event.job.completed 帧，用于校验 failed 事件带 error
  std::mutex notifyMutex;
  std::vector<json::JsonValue> completedEvents;
  JobManager::NotifyFn notify = [&](const std::string& method, const json::JsonValue& params) {
    if (method != "event.job.completed") return;
    std::lock_guard<std::mutex> lock(notifyMutex);
    completedEvents.push_back(params);
  };
  JobManager jobs(&pool, notify);

  auto lastCompletedFor = [&](int64_t jobId) -> json::JsonValue {
    std::lock_guard<std::mutex> lock(notifyMutex);
    for (auto it = completedEvents.rbegin(); it != completedEvents.rend(); ++it) {
      if (it->get("jobId").asInt(-1) == jobId) return *it;
    }
    return json::JsonValue::nullValue();
  };

  // ---------------- J1：ctx.fail() → failed ----------------
  printf("\n[J1] ctx.fail() 落到 failed 终态并带 error\n");
  {
    const int64_t jobId = jobs.start("test.fail", [](JobContext& ctx) {
      ctx.progress(10, "干活中");
      json::JsonValue result = json::JsonValue::objectValue();
      result.setItem("error", "磁盘满");
      result.setItem("partial", static_cast<int64_t>(3));
      ctx.fail(result);
      ctx.progress(100, "failed: 磁盘满");
    });
    JobState st;
    const bool terminal = waitTerminal(&jobs, jobId, &st, 3000);
    check(terminal, "任务进入终态（jobId=" + std::to_string(jobId) + "）");
    check(st.state == "failed", "终态为 failed（实际 " + st.state + "）");
    check(st.errorCode != 0, "errorCode 非 0（实际 " + std::to_string(st.errorCode) + "）");
    check(st.errorMessage == "磁盘满", "errorMessage = 磁盘满（实际 " + st.errorMessage + "）");

    const json::JsonValue ev = lastCompletedFor(jobId);
    check(!ev.isNull(), "收到 event.job.completed");
    check(ev.get("state").asString() == "failed", "事件 state=failed");
    check(ev.get("error").get("message").asString() == "磁盘满",
          "事件 error.message 透传真因（实际 " +
              ev.get("error").get("message").asString() + "）");
  }

  // ---------------- J2：正常成功仍是 done ----------------
  printf("\n[J2] ctx.setResult() 依然判 done（不得误伤成功路径）\n");
  {
    const int64_t jobId = jobs.start("test.ok", [](JobContext& ctx) {
      ctx.progress(50, "一半");
      json::JsonValue result = json::JsonValue::objectValue();
      result.setItem("imported", static_cast<int64_t>(7));
      ctx.setResult(result);
      ctx.progress(100, "ok");
    });
    JobState st;
    check(waitTerminal(&jobs, jobId, &st, 3000), "任务进入终态");
    check(st.state == "done", "终态为 done（实际 " + st.state + "）");
    const json::JsonValue ev = lastCompletedFor(jobId);
    check(ev.get("result").get("imported").asInt(-1) == 7, "事件 result.imported=7");
  }

  // ---------------- J3：cancel 返回值 ----------------
  printf("\n[J3] cancel 不再恒回 true\n");
  {
    // 已结束（done）的任务
    const int64_t doneJob = jobs.start("test.quick", [](JobContext& ctx) {
      ctx.setResult(json::JsonValue::objectValue());
      ctx.progress(100, "done");
    });
    JobState st;
    check(waitTerminal(&jobs, doneJob, &st, 3000) && st.state == "done", "quick job 已完成");
    check(!jobs.cancel(doneJob), "cancel(已完成) == false");

    check(!jobs.cancel(987654), "cancel(不存在) == false");

    // 运行中的任务：用阻塞任务占住线程池
    std::atomic<bool> release{false};
    const int64_t running = jobs.start("test.block", [&release](JobContext& ctx) {
      ctx.progress(1, "等待放行");
      for (int i = 0; i < 300 && !ctx.isCancelled() && !release.load(); ++i) sleepMs(10);
    });
    // 等它真的进入 running（避免读到 queued 状态下的 cancelled 分支）
    bool sawRunning = false;
    for (int i = 0; i < 100 && !sawRunning; ++i) {
      JobState cur;
      jobs.getJob(running, &cur);
      if (cur.state == "running") sawRunning = true;
      else sleepMs(10);
    }
    check(sawRunning, "阻塞任务进入 running");
    check(jobs.cancel(running), "cancel(运行中) == true");
    release.store(true);
    JobState fin;
    check(waitTerminal(&jobs, running, &fin, 5000), "阻塞任务收尾");
    check(fin.state == "cancelled", "被取消任务终态为 cancelled（实际 " + fin.state + "）");
  }

  // ---------------- J4：不可访问源目录 → import job failed ----------------
  printf("\n[J4] 源目录不可访问 → 导入 job 终态为 failed（修复前是 done）\n");
  {
    db::PhotoDao photos(&database);
    db::FtsIndexer fts(&database);
    raw::RawDecoder decoder;
    xmp::XmpService xmp(&config, &photos, &database, &pool);
    db::ImageDao images(&database, &xmp);
    import::Importer importer(&config, &photos, &images, &database, &fts, &decoder, &xmp, notify);

    import::ImportOptions opts;
    // 故意给一个必然不存在的源目录
    opts.sourcePath = std::string(kHome) + "/no-such-source-dir";
    opts.targetPath = kPhotoDir;

    const int64_t jobId =
        jobs.start("import", [&importer, opts](JobContext& ctx) { importer.run(opts, ctx); });
    JobState st;
    check(waitTerminal(&jobs, jobId, &st, 10000), "导入 job 进入终态");
    check(st.state == "failed", "终态为 failed 而非 done（实际 " + st.state + "）");
    check(st.errorMessage.find("扫描失败") != std::string::npos ||
              st.errorMessage.find("source folder not found") != std::string::npos,
          "errorMessage 说明真因（实际 " + st.errorMessage + "）");
  }

  jobs.shutdown();
  pool.shutdown();

  printf("\n=== 结果：%s（失败 %d 项）===\n", gFailures == 0 ? "PASS" : "FAIL", gFailures);
  return gFailures == 0 ? 0 : 1;
}
