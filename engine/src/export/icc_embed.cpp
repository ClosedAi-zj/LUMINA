#include "export/icc_embed.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>

#include "core/str_util.h"

namespace lumina {
namespace icc_embed {
namespace {

bool readAll(const std::string& path, std::vector<uint8_t>* out, std::string* err) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    if (err) *err = "无法读取文件: " + path;
    return false;
  }
  out->assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
  if (out->size() < 4) {
    if (err) *err = "文件过短: " + path;
    return false;
  }
  return true;
}

/** 扫描 JPEG 段结构，找 APP2 + "ICC_PROFILE\0"（不解析熵编码数据，遇 SOS 即停） */
bool scanJpegIcc(const std::vector<uint8_t>& buf, bool* has, size_t* endOfHeaders,
                 std::string* err) {
  *has = false;
  *endOfHeaders = 0;
  if (buf.size() < 4 || buf[0] != 0xFF || buf[1] != 0xD8) {
    if (err) *err = "不是 JPEG（缺 SOI）";
    return false;
  }
  size_t p = 2;
  while (p + 4 <= buf.size()) {
    if (buf[p] != 0xFF) {
      if (err) *err = "JPEG 段结构错位（偏移 " + std::to_string(p) + "）";
      return false;
    }
    const uint8_t marker = buf[p + 1];
    if (marker == 0xD9 || marker == 0xDA) {  // EOI / SOS
      *endOfHeaders = p;
      return true;
    }
    // 无长度字段的独立标记
    if (marker == 0x01 || (marker >= 0xD0 && marker <= 0xD7) || marker == 0xFF) {
      p += 2;
      continue;
    }
    const size_t len = (static_cast<size_t>(buf[p + 2]) << 8) | buf[p + 3];
    if (len < 2 || p + 2 + len > buf.size()) {
      if (err) *err = "JPEG 段长度越界（偏移 " + std::to_string(p) + "）";
      return false;
    }
    if (marker == 0xE2 && len >= 2 + 12 + 2) {
      static const char kSig[12] = {'I', 'C', 'C', '_', 'P', 'R', 'O', 'F', 'I',
                                    'L', 'E', '\0'};
      if (std::memcmp(buf.data() + p + 4, kSig, 12) == 0) *has = true;
    }
    p += 2 + len;
  }
  if (err) *err = "JPEG 未找到 SOS 段";
  return false;
}

}  // namespace

bool jpegHasIcc(const std::string& path, bool* has, std::string* err) {
  std::vector<uint8_t> buf;
  if (!readAll(path, &buf, err)) return false;
  size_t endHeaders = 0;
  return scanJpegIcc(buf, has, &endHeaders, err);
}

bool injectJpegIcc(const std::string& path, const std::vector<uint8_t>& icc, std::string* err) {
  if (icc.empty()) {
    if (err) *err = "ICC 为空";
    return false;
  }
  std::vector<uint8_t> buf;
  if (!readAll(path, &buf, err)) return false;

  bool has = false;
  size_t endHeaders = 0;
  if (!scanJpegIcc(buf, &has, &endHeaders, err)) return false;
  if (has) {
    if (err) *err = "JPEG 已含 ICC_PROFILE APP2，拒绝重复嵌入: " + path;
    return false;
  }

  const size_t total =
      (icc.size() + kJpegChunkMax - 1) / kJpegChunkMax;  // 段数（1..255）
  if (total > 255) {
    if (err) *err = "ICC 过大，无法用 APP2 承载（段数 " + std::to_string(total) + "）";
    return false;
  }

  std::string out;
  out.reserve(buf.size() + icc.size() + total * 20);
  out.push_back(static_cast<char>(0xFF));
  out.push_back(static_cast<char>(0xD8));  // SOI

  for (size_t i = 0; i < total; ++i) {
    const size_t begin = i * kJpegChunkMax;
    const size_t chunk = std::min(kJpegChunkMax, icc.size() - begin);
    const uint16_t segLen = static_cast<uint16_t>(2 + 12 + 2 + chunk);
    out.push_back(static_cast<char>(0xFF));
    out.push_back(static_cast<char>(0xE2));
    out.push_back(static_cast<char>((segLen >> 8) & 0xFF));
    out.push_back(static_cast<char>(segLen & 0xFF));
    out.append("ICC_PROFILE\0", 12);
    out.push_back(static_cast<char>(i + 1));
    out.push_back(static_cast<char>(total));
    out.append(reinterpret_cast<const char*>(icc.data() + begin), chunk);
  }
  // 其余段（APP0/JFIF 等）原样搬
  out.append(reinterpret_cast<const char*>(buf.data() + 2), buf.size() - 2);

  return writeFileBytes(path, out, err);
}

}  // namespace icc_embed
}  // namespace lumina
