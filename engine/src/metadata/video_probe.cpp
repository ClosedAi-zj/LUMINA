// MP4/MOV 容器探测实现（ISO-BMFF box 遍历）
#include "metadata/video_probe.h"

#include <cstring>

#include "core/logger.h"
#include "core/str_util.h"

namespace lumina {
namespace metadata {
namespace {

// MP4 时间基准：1904-01-01 起算的秒
constexpr int64_t kMp4EpochOffsetMs = 2082844800000LL;

uint32_t be32(const uint8_t* p) {
  return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
         (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
}

uint64_t be64(const uint8_t* p) {
  return (static_cast<uint64_t>(be32(p)) << 32) | be32(p + 4);
}

struct Box {
  uint32_t size = 0;
  uint64_t largesize = 0;
  std::string type;
  size_t headerSize = 0;
  uint64_t payloadSize() const {
    if (largesize) return largesize - headerSize;
    return static_cast<uint64_t>(size) - headerSize;
  }
};

bool readBox(const uint8_t* data, size_t size, size_t pos, Box* box) {
  if (pos + 8 > size) return false;
  box->size = be32(data + pos);
  box->type = std::string(reinterpret_cast<const char*>(data + pos + 4), 4);
  box->largesize = 0;
  box->headerSize = 8;
  if (box->size == 1) {
    if (pos + 16 > size) return false;
    box->largesize = be64(data + pos + 8);
    box->headerSize = 16;
  } else if (box->size == 0) {
    // 直到文件尾
    box->largesize = size - pos;
    box->headerSize = 8;
  }
  return box->size >= 8 || box->largesize >= 16;
}

void parseTkhd(const uint8_t* data, size_t size, VideoInfo* out) {
  if (size < 84) return;
  uint8_t version = data[0];
  size_t widthOff = (version == 1) ? 96 : 84;
  if (widthOff + 8 > size) return;
  uint32_t w = be32(data + widthOff) >> 16;
  uint32_t h = be32(data + widthOff + 4) >> 16;
  if (w > 0) out->width = static_cast<int>(w);
  if (h > 0) out->height = static_cast<int>(h);
}

void parseMvhd(const uint8_t* data, size_t size, VideoInfo* out) {
  if (size < 20) return;
  uint8_t version = data[0];
  if (version == 1) {
    if (size < 32) return;
    uint32_t timescale = be32(data + 20);
    uint64_t duration = be64(data + 24);
    uint64_t creation = be64(data + 4);
    out->creationMs = static_cast<int64_t>(creation) * 1000 - kMp4EpochOffsetMs;
    if (timescale > 0) out->durationSec = static_cast<double>(duration) / timescale;
  } else {
    if (size < 24) return;
    uint32_t timescale = be32(data + 12);
    uint32_t duration = be32(data + 16);
    uint32_t creation = be32(data + 4);
    out->creationMs = static_cast<int64_t>(creation) * 1000 - kMp4EpochOffsetMs;
    if (timescale > 0) out->durationSec = static_cast<double>(duration) / timescale;
  }
}

void walk(const uint8_t* data, size_t size, size_t pos, size_t end, VideoInfo* out,
          bool insideMoov, int depth) {
  if (depth > 8) return;
  while (pos + 8 <= end) {
    Box box;
    if (!readBox(data, size, pos, &box)) return;
    if (box.headerSize == 0 || box.headerSize > size) return;
    uint64_t payload = box.payloadSize();
    if (payload == 0) break;
    size_t payloadPos = pos + box.headerSize;
    if (payloadPos + payload > size) return;
    if (box.type == "moov") {
      walk(data, size, payloadPos, payloadPos + payload, out, true, depth + 1);
    } else if (insideMoov && box.type == "trak") {
      walk(data, size, payloadPos, payloadPos + payload, out, true, depth + 1);
    } else if (insideMoov && box.type == "tkhd") {
      parseTkhd(data + payloadPos, static_cast<size_t>(payload), out);
    } else if (insideMoov && box.type == "mvhd") {
      parseMvhd(data + payloadPos, static_cast<size_t>(payload), out);
    } else if (insideMoov && box.type == "stsd" && payload >= 16) {
      // sample description 第一个条目的 format 字段
      size_t entryCount = be32(data + payloadPos + 4);
      if (entryCount > 0 && payload >= 24) {
        size_t entryPos = payloadPos + 8;
        if (entryPos + 8 <= size) {
          out->codec = std::string(reinterpret_cast<const char*>(data + entryPos + 4), 4);
        }
      }
    }
    uint64_t advance = box.headerSize + payload;
    if (advance == 0) break;
    pos += static_cast<size_t>(advance);
  }
}

}  // namespace

bool probeVideo(const std::string& path, VideoInfo* out, std::string* err) {
  std::string bytes;
  if (!readFile(path, &bytes, err)) return false;
  if (bytes.size() < 16) {
    if (err) *err = "file too small for mp4: " + path;
    return false;
  }
  const uint8_t* data = reinterpret_cast<const uint8_t*>(bytes.data());
  if (!(bytes.size() >= 12 && std::memcmp(data + 4, "ftyp", 4) == 0)) {
    if (err) *err = "not an mp4/mov container: " + path;
    return false;
  }
  out->fileSize = static_cast<int64_t>(bytes.size());
  walk(data, bytes.size(), 0, bytes.size(), out, false, 0);
  if (out->width == 0 && out->height == 0 && out->durationSec <= 0.0) {
    if (err) *err = "mp4 parse produced no metadata: " + path;
    return false;
  }
  return true;
}

}  // namespace metadata
}  // namespace lumina
