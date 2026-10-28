#include "export/icc_embed.h"

#include <algorithm>
#include <cstddef>
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

// ---------------------------------------------------------------------------
// PNG 基础件：CRC-32 / Adler-32 / 大端读写 / 最小 zlib(stored) 编码
// ---------------------------------------------------------------------------

/** PNG 签名（PNG spec 5.2） */
constexpr uint8_t kPngSignature[8] = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};

/**
 * CRC-32 查表（PNG spec 5.5：标准多项式 0xEDB88320，反射算法，初值 0xFFFFFFFF，末尾取反）。
 * 表在首次使用时构造一次：C++11 起函数内 static 局部变量的初始化是线程安全的，
 * 因此多线程并发调用 injectPngIcc / pngHasIcc 不需要额外加锁。
 */
struct Crc32Table {
  uint32_t v[256];
  Crc32Table() {
    for (uint32_t n = 0; n < 256; ++n) {
      uint32_t c = n;
      for (int k = 0; k < 8; ++k) {
        c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
      }
      v[n] = c;
    }
  }
};

const uint32_t* crc32Table() {
  static const Crc32Table table;  // C++11：magic static，线程安全
  return table.v;
}

uint32_t pngCrc32(const uint8_t* data, size_t len) {
  const uint32_t* table = crc32Table();
  uint32_t c = 0xFFFFFFFFu;
  for (size_t i = 0; i < len; ++i) {
    c = table[(c ^ data[i]) & 0xFFu] ^ (c >> 8);
  }
  return c ^ 0xFFFFFFFFu;
}

/** Adler-32（RFC 1950 §9）：a=1,b=0 起，逐字节累加，模 65521，输出 (b<<16)|a */
uint32_t adler32(const uint8_t* data, size_t len) {
  uint32_t a = 1;
  uint32_t b = 0;
  for (size_t i = 0; i < len; ++i) {
    a = (a + data[i]) % 65521u;
    b = (b + a) % 65521u;
  }
  return (b << 16) | a;
}

void putBe32(std::vector<uint8_t>* out, uint32_t v) {
  out->push_back(static_cast<uint8_t>((v >> 24) & 0xFFu));
  out->push_back(static_cast<uint8_t>((v >> 16) & 0xFFu));
  out->push_back(static_cast<uint8_t>((v >> 8) & 0xFFu));
  out->push_back(static_cast<uint8_t>(v & 0xFFu));
}

uint32_t getBe32(const uint8_t* p) {
  return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
         (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
}

/** 8 位大写十六进制（CRC 不符时的错误信息用，便于对账） */
std::string hex32(uint32_t v) {
  char buf[9] = {0};
  std::snprintf(buf, sizeof(buf), "%08X", v);
  return std::string(buf);
}

/**
 * 最小 zlib 流编码：只用 **stored（未编码）块**（RFC 1951 §3.2.4），不做 Huffman 压缩。
 *
 * 取舍说明（不是偷懒，是明确的体积换依赖）：
 *  - 项目没有 zlib 依赖，且不允许为此引入第三方库；
 *  - ICC profile 通常只有几百到几千字节，stored 块相对固定/动态 Huffman 的膨胀可忽略
 *    （每 64KB 约 +5 字节块头），换来"零第三方依赖 + 100% 解码器可读"；
 *  - 任何符合 RFC 1950/1951 的解码器（libpng、zlib、PIL…）都能读 stored 块。
 *
 * 布局：CMF=0x78（CM=8 deflate, CINFO=7 即 32K 窗口）+ FLG=0x01
 *      （FCHECK 使 (CMF<<8|FLG)%31==0，FDICT=0，FLEVEL=0）
 *      + 若干 stored 块（BFINAL/BTYPE=0x00 或末块 0x01 + LEN(LE) + NLEN(LE) + 原文）
 *      + Adler-32（大端）。
 */
void zlibStoreRaw(const std::vector<uint8_t>& src, std::vector<uint8_t>* out) {
  out->push_back(0x78);  // CMF
  out->push_back(0x01);  // FLG：(0x7801 % 31) == 0，FDICT=0
  const size_t kMaxBlock = 65535;
  size_t off = 0;
  do {
    const size_t n = std::min(kMaxBlock, src.size() - off);
    const bool final = (off + n >= src.size());
    out->push_back(final ? 0x01 : 0x00);  // BFINAL(1 bit) + BTYPE=00 stored(2 bit)
    out->push_back(static_cast<uint8_t>(n & 0xFFu));         // LEN 低字节
    out->push_back(static_cast<uint8_t>((n >> 8) & 0xFFu));  // LEN 高字节
    const uint16_t nlen = static_cast<uint16_t>(~static_cast<uint16_t>(n));
    out->push_back(static_cast<uint8_t>(nlen & 0xFFu));         // NLEN（LEN 按位取反）
    out->push_back(static_cast<uint8_t>((nlen >> 8) & 0xFFu));  // NLEN 高字节
    out->insert(out->end(), src.begin() + static_cast<std::ptrdiff_t>(off),
                src.begin() + static_cast<std::ptrdiff_t>(off + n));
    off += n;
  } while (off < src.size());
  putBe32(out, adler32(src.data(), src.size()));
}

/** PNG chunk 位置信息（offset 指 length 字段起点） */
struct PngChunk {
  char type[5] = {0, 0, 0, 0, 0};
  size_t offset = 0;      // chunk 起始（length 字段）
  size_t dataOffset = 0;  // data 字段起始
  uint32_t length = 0;    // data 字节数
};

/**
 * 遍历 PNG 全部 chunk 并**逐个校验 CRC**（CRC 覆盖 type+data，不覆盖 length）。
 * 任一 chunk CRC 不符 / 长度越界都返回 false 并带上偏移与 chunk 类型 —— 不允许坏 CRC 静默放过。
 */
bool scanPngChunks(const std::vector<uint8_t>& buf, std::vector<PngChunk>* chunks,
                   std::string* err) {
  chunks->clear();
  if (buf.size() < 8 || std::memcmp(buf.data(), kPngSignature, 8) != 0) {
    if (err) *err = "不是 PNG（签名不匹配）";
    return false;
  }
  size_t p = 8;
  while (p + 12 <= buf.size()) {
    const uint32_t len = getBe32(&buf[p]);
    const size_t dataBegin = p + 8;
    const size_t crcBegin = dataBegin + len;
    if (crcBegin + 4 > buf.size()) {
      PngChunk partial;
      std::memcpy(partial.type, &buf[p + 4], 4);
      if (err) {
        *err = "PNG chunk 长度越界（偏移 " + std::to_string(p) + "，类型 \"" +
               std::string(partial.type) + "\"，声明长度 " + std::to_string(len) + "）";
      }
      return false;
    }
    // CRC 覆盖 type + data（即 buf[p+4 .. crcBegin)）
    const uint32_t expect = getBe32(&buf[crcBegin]);
    const uint32_t actual = pngCrc32(&buf[p + 4], static_cast<size_t>(len) + 4);
    if (expect != actual) {
      PngChunk bad;
      std::memcpy(bad.type, &buf[p + 4], 4);
      if (err) {
        *err = "PNG chunk CRC 校验失败（偏移 " + std::to_string(p) + "，类型 \"" +
               std::string(bad.type) + "\"，文件内 0x" + hex32(expect) + "，实算 0x" +
               hex32(actual) + "）";
      }
      return false;
    }
    PngChunk c;
    std::memcpy(c.type, &buf[p + 4], 4);
    c.offset = p;
    c.dataOffset = dataBegin;
    c.length = len;
    chunks->push_back(c);
    if (std::memcmp(c.type, "IEND", 4) == 0) {
      return true;
    }
    p = crcBegin + 4;
  }
  if (err) *err = "PNG 未找到 IEND（文件被截断）";
  return false;
}

/** 拼 iCCP 的 data 字段：profileName + '\0' + compressionMethod(0) + zlib(profile) */
void buildIccpData(const std::vector<uint8_t>& icc, std::vector<uint8_t>* data) {
  data->clear();
  const char* name = kPngProfileName;
  const size_t nameLen = std::strlen(name);
  data->insert(data->end(), name, name + nameLen);  // ASCII 名（1..79 字节，不含 NUL）
  data->push_back(0x00);                            // profileName 结尾的 NUL
  data->push_back(0x00);                            // compressionMethod：0 = zlib/deflate
  zlibStoreRaw(icc, data);
}

/** 追加一个完整 chunk：length(大端) + type + data + crc(覆盖 type+data，大端) */
void appendChunk(const char* type, const std::vector<uint8_t>& data, std::vector<uint8_t>* out) {
  putBe32(out, static_cast<uint32_t>(data.size()));
  const size_t typeBegin = out->size();
  out->insert(out->end(), type, type + 4);
  out->insert(out->end(), data.begin(), data.end());
  // CRC 覆盖 type + data：从 typeBegin 到当前末尾
  putBe32(out, pngCrc32(out->data() + typeBegin, out->size() - typeBegin));
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

bool pngHasIcc(const std::string& path, bool* has, std::string* err) {
  if (has) *has = false;
  std::vector<uint8_t> buf;
  if (!readAll(path, &buf, err)) return false;
  std::vector<PngChunk> chunks;
  // 即使提前遇到 iCCP 也要扫完全部 chunk：坏 CRC 必须暴露，不允许被"已找到"掩盖
  if (!scanPngChunks(buf, &chunks, err)) return false;
  for (const auto& c : chunks) {
    if (std::memcmp(c.type, "iCCP", 4) == 0) {
      if (has) *has = true;
      break;
    }
  }
  return true;
}

bool injectPngIcc(const std::string& path, const std::vector<uint8_t>& icc, std::string* err) {
  auto setErr = [&](const std::string& detail) {
    if (err) *err = "PNG iCCP 嵌入失败: " + detail;
  };
  if (icc.empty()) {
    setErr("ICC 为空");
    return false;
  }
  std::vector<uint8_t> buf;
  std::string readErr;
  if (!readAll(path, &buf, &readErr)) {
    setErr(readErr);
    return false;
  }

  std::vector<PngChunk> chunks;
  std::string scanErr;
  if (!scanPngChunks(buf, &chunks, &scanErr)) {
    setErr(scanErr);
    return false;
  }
  if (chunks.empty() || std::memcmp(chunks[0].type, "IHDR", 4) != 0) {
    setErr("PNG 首个 chunk 不是 IHDR");
    return false;
  }
  for (const auto& c : chunks) {
    if (std::memcmp(c.type, "iCCP", 4) == 0) {
      setErr("PNG 已含 iCCP chunk，拒绝重复嵌入: " + path);
      return false;
    }
    // PNG spec 11.3.3.5：iCCP 与 sRGB chunk 互斥（同时存在即非法文件）。宁可失败也不写坏文件。
    if (std::memcmp(c.type, "sRGB", 4) == 0) {
      setErr("PNG 已含 sRGB chunk，与 iCCP 互斥，拒绝写入非法文件: " + path);
      return false;
    }
  }

  std::vector<uint8_t> iccpData;
  buildIccpData(icc, &iccpData);
  std::vector<uint8_t> iccpChunk;
  iccpChunk.reserve(iccpData.size() + 12);
  appendChunk("iCCP", iccpData, &iccpChunk);

  /**
   * 插入位置：紧跟 IHDR 之后（IHDR 是 PNG 规范强制首块，必然存在）。
   * 规范（PNG 3rd ed. §11.3.3.3）要求 iCCP 出现在 IDAT 之前且不得位于 PLTE 之后；
   * 紧跟 IHDR 同时满足两条件（会把它推到 PLTE 之前，这正是规范期望的顺序）。
   */
  const size_t insertAt = chunks[0].offset + 12 + static_cast<size_t>(chunks[0].length);
  if (insertAt > buf.size()) {
    setErr("IHDR 长度异常，无法定位插入点");
    return false;
  }

  std::string out;
  out.reserve(buf.size() + iccpChunk.size());
  out.append(reinterpret_cast<const char*>(buf.data()), insertAt);
  out.append(reinterpret_cast<const char*>(iccpChunk.data()), iccpChunk.size());
  out.append(reinterpret_cast<const char*>(buf.data() + insertAt), buf.size() - insertAt);

  std::string writeErr;
  if (!writeFileBytes(path, out, &writeErr)) {
    setErr(writeErr);
    return false;
  }
  return true;
}

}  // namespace icc_embed
}  // namespace lumina
