// 方法注册表与分发：参数校验、错误码封装（未识别方法返回 -32601，绝不崩溃）
#ifndef LUMINA_RPC_DISPATCHER_H_
#define LUMINA_RPC_DISPATCHER_H_

#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "json/json_value.h"

namespace lumina {
namespace rpc {

// 业务错误（引擎侧统一出口）
struct RpcError {
  int code = 0;
  std::string message;
  json::JsonValue data;

  RpcError() : code(0), data(json::JsonValue::nullValue()) {}
  RpcError(int c, const std::string& m) : code(c), message(m), data(json::JsonValue::nullValue()) {}
  RpcError(int c, const std::string& m, const json::JsonValue& d) : code(c), message(m), data(d) {}
  bool ok() const { return code == 0; }
};

// 标准错误码（ipc-protocol §3）
constexpr int kParseError = -32700;
constexpr int kInvalidRequest = -32600;
constexpr int kMethodNotFound = -32601;
constexpr int kInvalidParams = -32602;
constexpr int kInternalError = -32603;
constexpr int kFileNotFound = 1001;
constexpr int kIoError = 1002;
constexpr int kDbError = 1003;
constexpr int kXmpParseError = 1004;
constexpr int kXmpExternalConflict = 1005;
constexpr int kSchemaTooNew = 1006;
constexpr int kXmpWriteFailed = 1007;
constexpr int kRawDecodeFailed = 2001;
constexpr int kUnsupportedFormat = 2002;
constexpr int kProxyMissing = 2003;
constexpr int kJobNotFound = 3001;
constexpr int kJobBusy = 3002;
constexpr int kJobCancelled = 3003;
constexpr int kJobFailed = 3004;
constexpr int kInsufficientSpace = 4001;
constexpr int kMd5Mismatch = 4002;
constexpr int kSourceUnreadable = 4003;
constexpr int kCodecUnavailable = 5001;
constexpr int kEngineShuttingDown = 5002;
constexpr int kGalleryConflict = 6001;

// 分发结果
struct RpcResponse {
  bool isError = false;
  json::JsonValue result;
  int code = 0;
  std::string message;
  json::JsonValue data;

  json::JsonValue toEnvelope(const json::JsonValue& id) const;
};

class Dispatcher {
 public:
  using Handler = std::function<json::JsonValue(const json::JsonValue& params, RpcError* err)>;

  void registerMethod(const std::string& name, Handler handler);
  bool hasMethod(const std::string& name) const;
  std::vector<std::string> methodNames() const;

  // 分发：命中 → 执行；未注册 → -32601；处理器抛异常 → -32603
  RpcResponse dispatch(const std::string& method, const json::JsonValue& params) const;

 private:
  mutable std::mutex mutex_;
  std::map<std::string, Handler> handlers_;
};

}  // namespace rpc
}  // namespace lumina

#endif  // LUMINA_RPC_DISPATCHER_H_
