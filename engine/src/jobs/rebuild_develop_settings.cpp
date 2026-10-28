// M2 T2.2b⑥ · 启动回填 job 实现：首跑/增量判定 + JobManager 提交
//
// 低优先级实现（线程池无优先级队列，见 core/thread_pool.h）：批间 sleep 让出 CPU。
// 每批 50 张（kBackfillChunk）、批间 20ms；有活干时单 worker 占空比 <100%（其余
// worker 与主线程照常服务 RPC/首帧）；无活干时（常规启动：缓存齐全且 mtime 未变）
// 只做每照片一次 stat 比对，0 解析 0 写库，开销可忽略。实测依据见
// tests/test_backfill_job.cpp 的 [P] 段（全量 130 张含解析+写库的总耗时打印）。
#include "jobs/rebuild_develop_settings.h"

#include <chrono>
#include <thread>

#include "core/job_manager.h"
#include "core/logger.h"
#include "core/str_util.h"
#include "db/develop_settings_dao.h"

namespace lumina {
namespace jobs {
namespace {

constexpr int kChunkGapMs = 20;  // 批间让出时长（低优先级节流，见文件头说明）

}  // namespace

json::JsonValue rebuildResultToJson(const RebuildJobResult& r) {
  json::JsonValue j = json::JsonValue::objectValue();
  j.setItem("mode", r.mode);
  j.setItem("scanned", r.scanned);
  j.setItem("written", r.written);
  j.setItem("removed", r.removed);
  j.setItem("failed", r.failed);
  j.setItem("cancelled", r.cancelled);
  j.setItem("durationMs", json::JsonValue::integer(r.durationMs));
  return j;
}

RebuildJobResult runRebuildDevelopSettings(db::Database* db, xmp::XmpService* xmp,
                                           std::atomic<bool>* cancel,
                                           const std::function<void(int, const std::string&)>&
                                               progress,
                                           std::string* err) {
  RebuildJobResult out;
  const int64_t startedMs = nowMs();
  db::DevelopSettingsDao dao(db);

  std::string availErr;
  if (!dao.available(&availErr)) {
    // 零静默：缓存层不可用必须留痕，不得当作「无调整」处理
    out.mode = "unavailable";
    logWarn("jobs", "启动回填跳过：develop_settings 不可用（" + availErr + "）");
    if (err) *err = availErr;
    out.durationMs = nowMs() - startedMs;
    return out;
  }

  int64_t rows = 0;
  std::string cntErr;
  if (!dao.cachedRowCount(&rows, &cntErr)) {
    out.mode = "unavailable";
    logWarn("jobs", "启动回填失败：develop_settings 行数查询失败（" + cntErr + "）");
    if (err) *err = cntErr;
    out.durationMs = nowMs() - startedMs;
    return out;
  }

  auto yieldFn = []() { std::this_thread::sleep_for(std::chrono::milliseconds(kChunkGapMs)); };
  db::RebuildReport rep;
  bool ok = false;
  if (rows == 0) {
    out.mode = "full";
    logInfo("jobs", "启动回填：develop_settings 为空（首跑/升级库），执行全量重建");
    ok = dao.rebuildFromSidecar(xmp, &rep, cancel, progress, err, yieldFn);
  } else {
    out.mode = "incremental";
    ok = dao.backfillFromSidecar(xmp, &rep, cancel, progress, err, yieldFn);
  }

  out.scanned = rep.scanned;
  out.written = rep.written;
  out.removed = rep.removed;
  out.failed = rep.failed;
  out.cancelled = rep.cancelled;
  out.durationMs = nowMs() - startedMs;

  // 失败不静默：硬失败（非取消）与失败行数都必须显式留痕，不得当「无调整」处理
  if (!ok && !out.cancelled) {
    logError("jobs", "启动回填失败（mode=" + out.mode + "）: " + (err ? *err : std::string()));
  }
  if (out.failed > 0) {
    logWarn("jobs", "启动回填存在失败行：failed=" + std::to_string(out.failed) +
                        "（mode=" + out.mode + "，原因见 db 日志）");
  }
  return out;
}

int64_t submitRebuildDevelopSettings(JobManager* jobs, db::Database* db,
                                     xmp::XmpService* xmp) {
  return jobs->start("rebuild_develop_settings", [db, xmp](JobContext& ctx) {
    // 取消桥接：JobContext 不暴露原生 cancel 指针，经 progress 回调（每批必发）
    // 同步到本地 stop，DAO 在批间检查——取消延迟 ≤ 一批（kBackfillChunk 张）。
    std::atomic<bool> stop{false};
    auto progress = [&ctx, &stop](int pct, const std::string& detail) {
      if (ctx.isCancelled()) stop.store(true);
      ctx.progress(pct, detail);
    };
    std::string runErr;
    const RebuildJobResult r = runRebuildDevelopSettings(db, xmp, &stop, progress, &runErr);
    if (r.cancelled) {
      logInfo("jobs", "启动回填 job 已取消（已判定 " + std::to_string(r.scanned) + "，已提交部分保持有效）");
    } else if (r.mode == "unavailable") {
      logWarn("jobs", "启动回填 job 结束：develop_settings 不可用（" + runErr + "）");
    } else {
      logInfo("jobs", "启动回填 job 完成（mode=" + r.mode + "，回填 " + std::to_string(r.scanned) +
                          "，写入 " + std::to_string(r.written) + "，清除 " +
                          std::to_string(r.removed) + "，失败 " + std::to_string(r.failed) +
                          "，耗时 " + std::to_string(r.durationMs) + "ms）");
    }
    ctx.setResult(rebuildResultToJson(r));
  });
}

}  // namespace jobs
}  // namespace lumina
