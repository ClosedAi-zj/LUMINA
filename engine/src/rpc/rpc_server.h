// stdio JSON-RPC 服务端：帧读取主循环、响应/通知发送（stdout 互斥，日志永不混入）
#ifndef LUMINA_RPC_RPC_SERVER_H_
#define LUMINA_RPC_RPC_SERVER_H_

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>

#include "core/job_manager.h"
#include "json/json_value.h"
#include "rpc/dispatcher.h"

namespace lumina {
namespace rpc {

class RpcServer {
 public:
  explicit RpcServer(Dispatcher* dispatcher);

  // 主循环：读帧 → 解析 → 分发 → 回包；返回进程退出码
  int run();

  // 请求优雅退出（system.shutdown / SIGINT）
  void requestShutdown();
  bool isShuttingDown() const { return shutdownRequested_.load(); }
  void setShutdownHook(std::function<void()> hook);

  int64_t uptimeMs() const;

  // ---- 发送（线程安全：worker 线程可直接调用发通知） ----
  void sendResponse(const json::JsonValue& id, const json::JsonValue& result);
  void sendError(const json::JsonValue& id, int code, const std::string& message,
                 const json::JsonValue& data);
  void sendNotification(const std::string& method, const json::JsonValue& params);

  // 供 JobManager 使用的通知回调
  JobManager::NotifyFn notifyFn();

  // 已处理请求数（诊断用）
  int64_t requestCount() const { return requestCount_.load(); }

  // ---- 主循环状态（看门狗诊断用） ----
  enum LoopState {
    kLoopReading = 0,    // 阻塞在 stdin 读帧
    kLoopDispatching = 1,
    kLoopWritingReply = 2,
    kLoopLeaving = 3,    // 已决定退出主循环
    kLoopExited = 4,
  };
  void setLoopState(int s) { loopState_.store(s); }
  int loopState() const { return loopState_.load(); }

 private:
  void handleEnvelope(const json::JsonValue& envelope);
  void writeEnvelope(const json::JsonValue& envelope);

  Dispatcher* dispatcher_;
  std::mutex writeMutex_;
  std::atomic<bool> shutdownRequested_;
  std::atomic<int64_t> requestCount_;
  std::atomic<int> loopState_;
  int64_t startedAt_;
  std::function<void()> shutdownHook_;
};

}  // namespace rpc
}  // namespace lumina

#endif  // LUMINA_RPC_RPC_SERVER_H_
