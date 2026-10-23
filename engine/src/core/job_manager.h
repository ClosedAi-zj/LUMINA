// 异步任务管理：jobId 分配、取消标志、进度回调 → event.job.* 通知
#ifndef LUMINA_CORE_JOB_MANAGER_H_
#define LUMINA_CORE_JOB_MANAGER_H_

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "core/thread_pool.h"
#include "json/json_value.h"

namespace lumina {

// 任务上下文：任务内通过它上报进度并感知取消
class JobContext {
 public:
  JobContext(int64_t jobId, std::atomic<bool>* cancelFlag,
             std::function<void(int, const std::string&)> progress,
             std::function<void(const json::JsonValue&)> setResult)
      : jobId_(jobId), cancelFlag_(cancelFlag), progress_(std::move(progress)),
        setResult_(std::move(setResult)) {}

  int64_t jobId() const { return jobId_; }
  bool isCancelled() const { return cancelFlag_ ? cancelFlag_->load() : false; }
  // progress: 0..100；detail 为可读文案
  void progress(int percent, const std::string& detail) {
    if (progress_) progress_(percent, detail);
  }
  // 设置完成事件携带的 result（协议 §6 event.job.completed）
  void setResult(const json::JsonValue& result) {
    if (setResult_) setResult_(result);
  }

 private:
  int64_t jobId_;
  std::atomic<bool>* cancelFlag_;
  std::function<void(int, const std::string&)> progress_;
  std::function<void(const json::JsonValue&)> setResult_;
};

// 任务状态（对应 ipc-protocol §4 JobInfo）
struct JobState {
  int64_t jobId = 0;
  std::string kind;
  std::string state = "queued";  // queued|running|done|failed|cancelled
  int progress = 0;
  std::string detail;
  int64_t etaMs = 0;
  bool hasEta = false;
  int64_t startedAt = 0;
  int64_t finishedAt = 0;
  json::JsonValue result;  // 完成时附加
  int errorCode = 0;
  std::string errorMessage;

  json::JsonValue toJson() const;
};

class JobManager {
 public:
  using NotifyFn = std::function<void(const std::string& method, const json::JsonValue& params)>;
  using Task = std::function<void(JobContext&)>;

  JobManager(core::ThreadPool* pool, NotifyFn notify);

  // 启动任务，立即返回 jobId（协议：长任务立即返回，不占 UI 10s 超时）
  int64_t start(const std::string& kind, Task task);
  bool cancel(int64_t jobId);
  bool getJob(int64_t jobId, JobState* out) const;
  std::vector<JobState> list(const std::string& stateFilter) const;
  bool isCancelled(int64_t jobId) const;
  void shutdown();

 private:
  struct JobEntry {
    JobState state;
    std::shared_ptr<std::atomic<bool>> cancelFlag;
    // 进度通知节流（WP-A2）：state 恒实时更新，通知按 100ms 窗口合并（首末必发）
    int64_t lastEmitMs = 0;
    int lastEmitPercent = -1;
  };

  void emitProgress(const JobState& state);
  void emitCompleted(const JobState& state);

  mutable std::mutex mutex_;
  std::map<int64_t, JobEntry> jobs_;
  int64_t nextId_ = 1;
  core::ThreadPool* pool_;
  NotifyFn notify_;
  std::atomic<bool> shuttingDown_;
};

}  // namespace lumina

#endif  // LUMINA_CORE_JOB_MANAGER_H_
