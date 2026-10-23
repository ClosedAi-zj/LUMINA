// 方法注册表与分发实现
#include "rpc/dispatcher.h"

#include "core/logger.h"

namespace lumina {
namespace rpc {

json::JsonValue RpcResponse::toEnvelope(const json::JsonValue& id) const {
  json::JsonValue env = json::JsonValue::objectValue();
  env.setItem("jsonrpc", "2.0");
  env.setItem("id", id);
  if (isError) {
    json::JsonValue error = json::JsonValue::objectValue();
    error.setItem("code", code);
    error.setItem("message", message);
    if (!data.isNull()) error.setItem("data", data);
    env.setItem("error", error);
  } else {
    env.setItem("result", result.isNull() ? json::JsonValue::objectValue() : result);
  }
  return env;
}

void Dispatcher::registerMethod(const std::string& name, Handler handler) {
  std::lock_guard<std::mutex> lock(mutex_);
  handlers_[name] = std::move(handler);
}

bool Dispatcher::hasMethod(const std::string& name) const {
  std::lock_guard<std::mutex> lock(mutex_);
  return handlers_.find(name) != handlers_.end();
}

std::vector<std::string> Dispatcher::methodNames() const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<std::string> out;
  for (const auto& kv : handlers_) out.push_back(kv.first);
  return out;
}

RpcResponse Dispatcher::dispatch(const std::string& method, const json::JsonValue& params) const {
  Handler handler;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = handlers_.find(method);
    if (it == handlers_.end()) {
      RpcResponse resp;
      resp.isError = true;
      resp.code = kMethodNotFound;
      resp.message = "method not found: " + method;
      logWarn("rpc", resp.message);
      return resp;
    }
    handler = it->second;
  }
  RpcError err;
  json::JsonValue result;
  try {
    result = handler(params.isNull() ? json::JsonValue::objectValue() : params, &err);
  } catch (const std::exception& ex) {
    RpcResponse resp;
    resp.isError = true;
    resp.code = kInternalError;
    resp.message = std::string("internal error: ") + ex.what();
    json::JsonValue data = json::JsonValue::objectValue();
    data.setItem("stack", resp.message);
    resp.data = data;
    logError("rpc", "dispatch " + method + " threw: " + resp.message);
    return resp;
  } catch (...) {
    RpcResponse resp;
    resp.isError = true;
    resp.code = kInternalError;
    resp.message = "internal error: unknown exception in " + method;
    logError("rpc", resp.message);
    return resp;
  }
  RpcResponse resp;
  if (!err.ok()) {
    resp.isError = true;
    resp.code = err.code;
    resp.message = err.message;
    resp.data = err.data;
    return resp;
  }
  resp.result = result;
  return resp;
}

}  // namespace rpc
}  // namespace lumina
