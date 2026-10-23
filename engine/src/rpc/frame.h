// IPC 帧编解码：4 字节小端长度前缀 + UTF-8 JSON 载荷（单帧 ≤ 8MiB）
#ifndef LUMINA_RPC_FRAME_H_
#define LUMINA_RPC_FRAME_H_

#include <cstdint>
#include <string>

namespace lumina {
namespace rpc {

// 单帧上限 8MiB（超限即断开并记日志）
constexpr uint32_t kMaxFrameSize = 8u * 1024u * 1024u;

// 把 4 字节小端长度头写入 out
void encodeFrameHeader(uint32_t length, std::string* out);

// 完整帧 = 头 + 载荷
std::string encodeFrame(const std::string& payload);

// 从 stdin 读一帧；eof=true 表示对端关闭管道
bool readFrame(std::string* payload, bool* eof, std::string* err);

// 向 stdout 写一帧（二进制模式，线程安全由调用方保证）
bool writeFrame(const std::string& payload, std::string* err);

// 进程启动时把 stdin/stdout 切到二进制模式（Windows 必须，否则 0x1A/0x0A 会被改写）
void initStdioBinaryMode();

}  // namespace rpc
}  // namespace lumina

#endif  // LUMINA_RPC_FRAME_H_
