// 固定大小线程池（header-only）：引擎长任务（解码/导入/扫描）统一走这里
#ifndef LUMINA_CORE_THREAD_POOL_H_
#define LUMINA_CORE_THREAD_POOL_H_

#include <atomic>
#include <condition_variable>
#include <exception>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>

#include "core/logger.h"

namespace lumina {
namespace core {

class ThreadPool {
 public:
  explicit ThreadPool(size_t threadCount) : stop_(false), active_(0) {
    if (threadCount == 0) threadCount = 1;
    for (size_t i = 0; i < threadCount; ++i) {
      workers_.emplace_back([this]() { this->workerLoop(); });
    }
  }

  ~ThreadPool() { shutdown(); }

  ThreadPool(const ThreadPool&) = delete;
  ThreadPool& operator=(const ThreadPool&) = delete;

  // 提交任务；返回 future（调用方不必等待，丢弃 future 亦可）
  template <typename F>
  auto submit(F&& func) -> std::future<decltype(func())> {
    using ResultType = decltype(func());
    auto task = std::make_shared<std::packaged_task<ResultType()>>(std::forward<F>(func));
    std::future<ResultType> fut = task->get_future();
    bool runInline = false;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (stop_) {
        runInline = true;
      } else {
        queue_.emplace([task]() { (*task)(); });
      }
    }
    if (runInline) {
      // 已关闭：就地执行，保证调用方拿到结果而不丢任务。
      // ⚠ 必须在解锁之后执行：持锁内联运行用户任务会让任务内任何再次 submit()
      // 直接自死锁（std::mutex 不可重入），也会拖住 shutdown()/pendingCount()。
      (*task)();
      return fut;
    }
    cv_.notify_one();
    return fut;
  }

  size_t pendingCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return queue_.size();
  }

  size_t threadCount() const { return workers_.size(); }

  void shutdown() {
    std::queue<std::function<void()>> dropped;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (stop_) return;
      stop_ = true;
      // 未开始的任务不会被执行：必须显式留痕（此前是静默丢弃，调用方只看到
      // 一个永远不完成的 future / 永远 queued 的 job，没有任何线索）。
      dropped.swap(queue_);
    }
    if (!dropped.empty()) {
      logWarn("pool", "shutdown: dropped " + std::to_string(dropped.size()) +
                          " queued task(s) that never started");
    }
    cv_.notify_all();
    for (std::thread& t : workers_) {
      if (t.joinable()) t.join();
    }
    workers_.clear();
  }

 private:
  void workerLoop() {
    for (;;) {
      std::function<void()> job;
      {
      std::unique_lock<std::mutex> lock(mutex_);
      cv_.wait(lock, [this]() { return stop_ || !queue_.empty(); });
      if (stop_) return;  // 关闭即丢弃未开始的任务：shutdown 不能被积压队列拖住（进程退出红线）
      job = std::move(queue_.front());
        queue_.pop();
      }
      ++active_;
      try {
        job();
      } catch (const std::exception& e) {
        // 红线「零静默」：任务异常必须留痕，否则 job 会永远停在 running 且无任何线索
        logError("pool", std::string("task threw exception: ") + e.what());
      } catch (...) {
        logError("pool", "task threw unknown exception");
      }
      --active_;
    }
  }

  mutable std::mutex mutex_;
  std::condition_variable cv_;
  std::vector<std::thread> workers_;
  std::queue<std::function<void()>> queue_;
  bool stop_;
  std::atomic<int> active_;
};

}  // namespace core
}  // namespace lumina

#endif  // LUMINA_CORE_THREAD_POOL_H_
