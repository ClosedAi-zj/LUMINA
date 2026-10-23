// 异步任务管理实现：进度/完成事件保序发出，失败必带 error（红线：禁止吞异常）
#include "core/job_manager.h"

#include "core/logger.h"
#include "core/str_util.h"

namespace lumina {

json::JsonValue JobState::toJson() const {
  json::JsonValue j = json::JsonValue::objectValue();
  j.setItem("jobId", jobId);
  j.setItem("kind", kind);
  j.setItem("state", state);
  j.setItem("progress", progress);
  j.setItem("detail", detail);
  j.setItem("etaMs", hasEta ? json::JsonValue::integer(etaMs) : json::JsonValue::nullValue());
  return j;
}

JobManager::JobManager(core::ThreadPool* pool, NotifyFn notify)
    : pool_(pool), notify_(std::move(notify)), shuttingDown_(false) {}

int64_t JobManager::start(const std::string& kind, Task task) {
  JobEntry entry;
  int64_t jobId = 0;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    jobId = nextId_++;
    entry.state.jobId = jobId;
    entry.state.kind = kind;
    entry.state.state = "queued";
    entry.state.startedAt = nowMs();
    entry.cancelFlag = std::make_shared<std::atomic<bool>>(false);
    jobs_[jobId] = entry;
  }
  std::shared_ptr<std::atomic<bool>> cancelFlag = entry.cancelFlag;
  int64_t startedAt = entry.state.startedAt;

  pool_->submit([this, jobId, kind, cancelFlag, startedAt, task]() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      auto it = jobs_.find(jobId);
      if (it != jobs_.end()) it->second.state.state = "running";
    }
    auto progressFn = [this, jobId, startedAt](int percent, const std::string& detail) {
      bool shouldEmit = false;
      JobState snapshot;
      {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = jobs_.find(jobId);
        if (it == jobs_.end()) return;
        it->second.state.progress = percent;
        it->second.state.detail = detail;
        int64_t elapsed = nowMs() - startedAt;
        if (percent > 0 && elapsed > 0) {
          int64_t total = elapsed * 100 / percent;
          it->second.state.etaMs = total - elapsed;
          it->second.state.hasEta = true;
        }
        // 节流（WP-A2）：进度事件按 100ms 时间窗合并，避免细粒度任务刷屏拖慢 stdio；
        // 0% 与 100% 必发，保证 UI 的首态与终态不丢。
        int64_t now = nowMs();
        if (percent <= 0 || percent >= 100 || now - it->second.lastEmitMs >= 100) {
          shouldEmit = true;
          it->second.lastEmitMs = now;
          it->second.lastEmitPercent = percent;
        }
        snapshot = it->second.state;
      }
      // 在锁外发送：notify 写帧可能阻塞，不能拖住其他 job 的状态读写
      if (shouldEmit) emitProgress(snapshot);
    };
    auto setResultFn = [this, jobId](const json::JsonValue& result) {
      std::lock_guard<std::mutex> lock(mutex_);
      auto it = jobs_.find(jobId);
      if (it == jobs_.end()) return;
      it->second.state.result = result;
    };
    JobContext ctx(jobId, cancelFlag.get(), progressFn, setResultFn);
    int errorCode = 0;
    std::string errorMessage;
    try {
      task(ctx);
    } catch (const std::exception& ex) {
      errorCode = -32603;
      errorMessage = std::string("internal error: ") + ex.what();
      logError("job", "job " + std::to_string(jobId) + " exception: " + errorMessage);
    } catch (...) {
      errorCode = -32603;
      errorMessage = "internal error: unknown exception";
      logError("job", "job " + std::to_string(jobId) + " unknown exception");
    }

    JobState finalState;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      auto it = jobs_.find(jobId);
      if (it == jobs_.end()) return;
      if (cancelFlag->load()) {
        it->second.state.state = "cancelled";
      } else if (errorCode != 0) {
        it->second.state.state = "failed";
        it->second.state.errorCode = errorCode;
        it->second.state.errorMessage = errorMessage;
      } else {
        it->second.state.state = "done";
        it->second.state.progress = 100;
      }
      it->second.state.finishedAt = nowMs();
      it->second.state.hasEta = false;
      finalState = it->second.state;
    }
    emitCompleted(finalState);
  });
  return jobId;
}

bool JobManager::cancel(int64_t jobId) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = jobs_.find(jobId);
  if (it == jobs_.end()) return false;
  it->second.cancelFlag->store(true);
  if (it->second.state.state == "queued") it->second.state.state = "cancelled";
  return true;
}

bool JobManager::getJob(int64_t jobId, JobState* out) const {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = jobs_.find(jobId);
  if (it == jobs_.end()) return false;
  *out = it->second.state;
  return true;
}

std::vector<JobState> JobManager::list(const std::string& stateFilter) const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<JobState> out;
  for (const auto& kv : jobs_) {
    if (stateFilter.empty() || kv.second.state.state == stateFilter) out.push_back(kv.second.state);
  }
  return out;
}

bool JobManager::isCancelled(int64_t jobId) const {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = jobs_.find(jobId);
  if (it == jobs_.end()) return true;
  return it->second.cancelFlag->load();
}

void JobManager::emitProgress(const JobState& state) {
  if (!notify_) return;
  json::JsonValue params = json::JsonValue::objectValue();
  params.setItem("jobId", state.jobId);
  params.setItem("kind", state.kind);
  params.setItem("state", state.state);
  params.setItem("progress", state.progress);
  params.setItem("detail", state.detail);
  params.setItem("etaMs", state.hasEta ? json::JsonValue::integer(state.etaMs)
                                       : json::JsonValue::nullValue());
  notify_("event.job.progress", params);
}

void JobManager::emitCompleted(const JobState& state) {
  if (!notify_) return;
  json::JsonValue params = json::JsonValue::objectValue();
  params.setItem("jobId", state.jobId);
  params.setItem("kind", state.kind);
  params.setItem("state", state.state);
  params.setItem("progress", state.progress);
  params.setItem("detail", state.detail);
  if (state.state == "done" && !state.result.isNull()) {
    params.setItem("result", state.result);
  }
  if (state.state == "failed") {
    json::JsonValue error = json::JsonValue::objectValue();
    error.setItem("code", state.errorCode != 0 ? state.errorCode : 3004);
    error.setItem("message", state.errorMessage.empty() ? std::string("job failed")
                                                        : state.errorMessage);
    params.setItem("error", error);
  }
  notify_("event.job.completed", params);
}

void JobManager::shutdown() {
  shuttingDown_.store(true);
  std::lock_guard<std::mutex> lock(mutex_);
  for (auto& kv : jobs_) kv.second.cancelFlag->store(true);
}

}  // namespace lumina
