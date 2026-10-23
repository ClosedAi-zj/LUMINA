// 协议版本常量：与 app/src/api/protocol.ts 的 PROTOCOL_VERSION 必须逐字一致（CI 校验）
#ifndef LUMINA_RPC_VERSION_H_
#define LUMINA_RPC_VERSION_H_

namespace lumina {

// IPC 协议版本（docs/ipc-protocol.md）
constexpr const char* kProtocolVersion = "1.0";
// 协议主版本号（主版本不一致 = 破坏性变更，UI 必须降级退出）
constexpr int kProtocolMajor = 1;
constexpr int kProtocolMinor = 0;
// 引擎版本（随构建推进递增）
constexpr const char* kEngineVersion = "1.0.0";
// sidecar 当前 schema 版本（XMP 映射表 §7）
constexpr int kXmpSchemaVersion = 1;
// 认知的 crs:ProcessVersion 上限（超过则提示 1006，不猜测）
constexpr const char* kMaxProcessVersion = "15.4";

}  // namespace lumina

#endif  // LUMINA_RPC_VERSION_H_
