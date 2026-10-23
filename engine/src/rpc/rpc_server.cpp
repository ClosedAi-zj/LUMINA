// stdio JSON-RPC 服务端实现
#include "rpc/rpc_server.h"

#include "core/logger.h"
#include "core/str_util.h"
#include "rpc/frame.h"

namespace lumina {
namespace rpc {

namespace {
constexpr const char* kJsonRpcVersion = "2.0";
}  // namespace

RpcServer::RpcServer(Dispatcher* dispatcher)
    : dispatcher_(dispatcher), shutdownRequested_(false), requestCount_(0), loopState_(0),
      startedAt_(nowMs()) {}

int64_t RpcServer::uptimeMs() const { return nowMs() - startedAt_; }

void RpcServer::requestShutdown() { shutdownRequested_.store(true); }

void RpcServer::setShutdownHook(std::function<void()> hook) { shutdownHook_ = std::move(hook); }

void RpcServer::writeEnvelope(const json::JsonValue& envelope) {
  std::string payload = envelope.serialize();
  std::lock_guard<std::mutex> lock(writeMutex_);
  std::string err;
  if (!writeFrame(payload, &err)) {
    logError("rpc", "write frame failed: " + err);
  }
}

void RpcServer::sendResponse(const json::JsonValue& id, const json::JsonValue& result) {
  json::JsonValue env = json::JsonValue::objectValue();
  env.setItem("jsonrpc", kJsonRpcVersion);
  env.setItem("id", id);
  env.setItem("result", result.isNull() ? json::JsonValue::objectValue() : result);
  writeEnvelope(env);
}

void RpcServer::sendError(const json::JsonValue& id, int code, const std::string& message,
                          const json::JsonValue& data) {
  json::JsonValue env = json::JsonValue::objectValue();
  env.setItem("jsonrpc", kJsonRpcVersion);
  env.setItem("id", id);
  json::JsonValue error = json::JsonValue::objectValue();
  error.setItem("code", code);
  error.setItem("message", message);
  if (!data.isNull()) error.setItem("data", data);
  env.setItem("error", error);
  writeEnvelope(env);
}

void RpcServer::sendNotification(const std::string& method, const json::JsonValue& params) {
  // shutdown 之后不再外发通知：宿主可能已停止读取 stdout，管道缓冲写满会让
  // worker 线程阻塞在 writeFrame，导致 pool.shutdown() 永久 join 不完（进程退不出、锁不放）。
  if (shutdownRequested_.load()) return;
  json::JsonValue env = json::JsonValue::objectValue();
  env.setItem("jsonrpc", kJsonRpcVersion);
  env.setItem("method", method);
  env.setItem("params", params.isNull() ? json::JsonValue::objectValue() : params);
  writeEnvelope(env);
}

JobManager::NotifyFn RpcServer::notifyFn() {
  return [this](const std::string& method, const json::JsonValue& params) {
    this->sendNotification(method, params);
  };
}

void RpcServer::handleEnvelope(const json::JsonValue& envelope) {
  if (!envelope.isObject()) {
    sendError(json::JsonValue::nullValue(), kInvalidRequest, "envelope must be an object",
              json::JsonValue::nullValue());
    return;
  }
  json::JsonValue id = envelope.get("id");
  const json::JsonValue& method = envelope.get("method");
  if (!method.isString() || method.stringRef().empty()) {
    sendError(id, kInvalidRequest, "missing method", json::JsonValue::nullValue());
    return;
  }
  std::string name = method.stringRef();
  json::JsonValue params = envelope.get("params");
  setLoopState(kLoopDispatching);
  RpcResponse resp = dispatcher_->dispatch(name, params);
  writeEnvelope(resp.toEnvelope(id));
  requestCount_.fetch_add(1);
}

int RpcServer::run() {
  initStdioBinaryMode();
  logInfo("rpc", "rpc server started, waiting for frames on stdin");
  for (;;) {
    std::string payload;
    bool eof = false;
    std::string err;
    if (!readFrame(&payload, &eof, &err)) {
      if (eof) {
        logInfo("rpc", "stdin closed, exiting");
        return 0;
      }
      logError("rpc", "frame read failed: " + err + " (disconnecting)");
      return 1;
    }
    json::JsonValue envelope;
    std::string parseErr;
    if (!json::JsonValue::parse(payload, &envelope, &parseErr)) {
      logError("rpc", "json parse error: " + parseErr);
      sendError(json::JsonValue::nullValue(), kParseError, "json parse error: " + parseErr,
                json::JsonValue::nullValue());
      continue;
    }
    handleEnvelope(envelope);
    setLoopState(kLoopWritingReply);
    // handleEnvelope 内已写完响应；此处仅推进状态供看门狗观测
    if (shutdownRequested_.load()) {
      setLoopState(kLoopLeaving);
      logInfo("rpc", "shutdown requested, leaving main loop");
      if (shutdownHook_) shutdownHook_();
      setLoopState(kLoopExited);
      return 0;
    }
    setLoopState(kLoopReading);
  }
}

}  // namespace rpc
}  // namespace lumina
