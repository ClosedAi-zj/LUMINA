// IPC 帧编解码实现（stdio 二进制模式，避免 Windows 文本模式改写字节）
#include "rpc/frame.h"

#include <fcntl.h>
#include <io.h>
#include <cstdio>

#include "core/logger.h"

namespace lumina {
namespace rpc {

void initStdioBinaryMode() {
  _setmode(_fileno(stdin), _O_BINARY);
  _setmode(_fileno(stdout), _O_BINARY);
}

void encodeFrameHeader(uint32_t length, std::string* out) {
  out->push_back(static_cast<char>(length & 0xFF));
  out->push_back(static_cast<char>((length >> 8) & 0xFF));
  out->push_back(static_cast<char>((length >> 16) & 0xFF));
  out->push_back(static_cast<char>((length >> 24) & 0xFF));
}

std::string encodeFrame(const std::string& payload) {
  std::string out;
  out.reserve(payload.size() + 4);
  encodeFrameHeader(static_cast<uint32_t>(payload.size()), &out);
  out += payload;
  return out;
}

bool readFrame(std::string* payload, bool* eof, std::string* err) {
  *eof = false;
  payload->clear();
  unsigned char header[4];
  size_t got = fread(header, 1, 4, stdin);
  if (got == 0) {
    if (ferror(stdin)) {
      if (err) *err = "stdin read error";
      return false;
    }
    *eof = true;
    return false;
  }
  if (got != 4) {
    if (err) *err = "truncated frame header";
    return false;
  }
  uint32_t length = static_cast<uint32_t>(header[0]) | (static_cast<uint32_t>(header[1]) << 8) |
                    (static_cast<uint32_t>(header[2]) << 16) | (static_cast<uint32_t>(header[3]) << 24);
  if (length > kMaxFrameSize) {
    if (err) *err = "frame too large: " + std::to_string(length) + " bytes (max 8MiB)";
    return false;
  }
  if (length == 0) return true;
  payload->resize(length);
  size_t total = 0;
  while (total < length) {
    size_t n = fread(&(*payload)[total], 1, length - total, stdin);
    if (n == 0) {
      if (ferror(stdin)) {
        if (err) *err = "stdin read error while reading payload";
        return false;
      }
      *eof = true;
      return false;
    }
    total += n;
  }
  return true;
}

bool writeFrame(const std::string& payload, std::string* err) {
  std::string frame = encodeFrame(payload);
  size_t total = 0;
  while (total < frame.size()) {
    size_t n = fwrite(frame.data() + total, 1, frame.size() - total, stdout);
    if (n == 0) {
      if (err) *err = "stdout write error";
      return false;
    }
    total += n;
  }
  fflush(stdout);
  return true;
}

}  // namespace rpc
}  // namespace lumina
