// M4-02 回归：导出嵌入 ICC（JPEG APP2 / TIFF tag 34675 / PSD resource 0x040F / PNG iCCP）
// 判据：第三方解析（本文件内独立实现，不复用 writer 路径）读出的 profile 字节与写入值
//       **逐字节一致**，色彩空间签名正确，描述串正确。
// PNG 额外判据：chunk CRC 逐个独立校验、zlib(stored) 独立解码 + Adler-32 校验、
//       重复注入被拒绝、pngHasIcc 前后状态正确、坏 CRC 必须被检出；
//       另有 140000 B 大 profile 用例覆盖 stored **多块**路径（3 块，非末块 BFINAL=0）。
// 产物：m4-embed-report.json
#include <algorithm>
#include <cstdarg>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "color/icc.h"
#include "core/jpeg_encoder.h"
#include "export/exporter.h"
#include "export/icc_embed.h"
#include "export/psd_writer.h"
#include "imaging/imaging.h"

namespace {

int g_fail = 0;

void check(bool cond, const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  std::printf(cond ? "  \xE2\x9C\x93 " : "  \xE2\x9C\x97 ");
  std::vfprintf(stdout, fmt, ap);
  std::printf("\n");
  va_end(ap);
  if (!cond) ++g_fail;
}

bool readFile(const std::string& path, std::vector<uint8_t>* out) {
  FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) return false;
  std::fseek(f, 0, SEEK_END);
  const long n = std::ftell(f);
  std::fseek(f, 0, SEEK_SET);
  out->resize(static_cast<size_t>(n));
  const size_t rd = std::fread(out->data(), 1, out->size(), f);
  std::fclose(f);
  return rd == out->size();
}

uint32_t be32(const uint8_t* p) {
  return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
         (static_cast<uint32_t>(p[2]) << 8) | p[3];
}
uint32_t le32(const uint8_t* p) {
  return (static_cast<uint32_t>(p[3]) << 24) | (static_cast<uint32_t>(p[2]) << 16) |
         (static_cast<uint32_t>(p[1]) << 8) | p[0];
}
uint16_t le16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }

/** ICC 描述串（mcml/mluc/desc 三种类型都尽量解；失败返回空串） */
std::string iccDescription(const std::vector<uint8_t>& icc) {
  if (icc.size() < 132) return std::string();
  const uint32_t count = be32(&icc[128]);
  for (uint32_t i = 0; i < count; ++i) {
    const size_t off = 132 + static_cast<size_t>(i) * 12;
    if (off + 12 > icc.size()) break;
    if (std::memcmp(&icc[off], "desc", 4) != 0) continue;
    const uint32_t dataOff = be32(&icc[off + 4]);
    const uint32_t dataLen = be32(&icc[off + 8]);
    if (static_cast<size_t>(dataOff) + dataLen > icc.size() || dataLen < 12) break;
    const uint8_t* d = &icc[dataOff];
    if (std::memcmp(d, "mluc", 4) == 0) {
      const uint32_t records = be32(d + 8);
      if (records == 0) break;
      const uint32_t len = be32(d + 20);   // 第 1 条记录的长度
      const uint32_t roff = be32(d + 24);  // 第 1 条记录的偏移
      if (static_cast<size_t>(roff) + len > dataLen) break;
      std::string s;
      for (uint32_t k = 0; k + 1 < len; k += 2) {
        const uint16_t ch = static_cast<uint16_t>((d[roff + k] << 8) | d[roff + k + 1]);
        if (ch == 0) break;
        s.push_back(static_cast<char>(ch & 0x7F));
      }
      return s;
    }
    if (std::memcmp(d, "desc", 4) == 0) {
      const uint32_t slen = be32(d + 8);
      if (slen == 0 || 12 + slen > dataLen) break;
      return std::string(reinterpret_cast<const char*>(d + 12), slen - 1);
    }
  }
  return std::string();
}

/** 独立 JPEG 段解析：拼回 ICC_PROFILE APP2 片段 */
bool jpegExtractIcc(const std::string& path, std::vector<uint8_t>* icc, int* segments) {
  std::vector<uint8_t> buf;
  if (!readFile(path, &buf) || buf.size() < 4) return false;
  if (buf[0] != 0xFF || buf[1] != 0xD8) return false;
  std::vector<std::vector<uint8_t>> parts;
  size_t p = 2;
  *segments = 0;
  while (p + 4 <= buf.size()) {
    if (buf[p] != 0xFF) return false;
    const uint8_t marker = buf[p + 1];
    if (marker == 0xD9 || marker == 0xDA) break;
    if (marker == 0x01 || (marker >= 0xD0 && marker <= 0xD7) || marker == 0xFF) {
      p += 2;
      continue;
    }
    const size_t len = (static_cast<size_t>(buf[p + 2]) << 8) | buf[p + 3];
    if (len < 2 || p + 2 + len > buf.size()) return false;
    if (marker == 0xE2 && len > 16 && std::memcmp(&buf[p + 4], "ICC_PROFILE\0", 12) == 0) {
      const uint8_t seq = buf[p + 16];
      const uint8_t total = buf[p + 17];
      if (parts.size() < static_cast<size_t>(total)) parts.resize(total);
      parts[seq - 1].assign(buf.begin() + p + 18, buf.begin() + p + 2 + len);
      ++(*segments);
    }
    p += 2 + len;
  }
  icc->clear();
  for (auto& part : parts) icc->insert(icc->end(), part.begin(), part.end());
  return !icc->empty();
}

/** 独立 TIFF 解析：读 IFD 找 tag 34675 */
bool tiffExtractIcc(const std::string& path, std::vector<uint8_t>* icc) {
  std::vector<uint8_t> buf;
  if (!readFile(path, &buf) || buf.size() < 8) return false;
  if (buf[0] != 0x49 || buf[1] != 0x49 || le16(&buf[2]) != 42) return false;
  const uint32_t ifd = le32(&buf[4]);
  if (ifd + 2 > buf.size()) return false;
  const uint16_t n = le16(&buf[ifd]);
  for (uint16_t i = 0; i < n; ++i) {
    const size_t e = ifd + 2 + static_cast<size_t>(i) * 12;
    if (e + 12 > buf.size()) return false;
    if (le16(&buf[e]) != 34675) continue;
    const uint32_t count = le32(&buf[e + 4]);
    const uint32_t off = le32(&buf[e + 8]);
    if (static_cast<size_t>(off) + count > buf.size()) return false;
    icc->assign(buf.begin() + off, buf.begin() + off + count);
    return true;
  }
  return false;
}

/** 独立 PSD 解析：读 image resources 找 0x040F */
bool psdExtractIcc(const std::string& path, std::vector<uint8_t>* icc) {
  std::vector<uint8_t> buf;
  if (!readFile(path, &buf) || buf.size() < 34) return false;
  if (std::memcmp(buf.data(), "8BPS", 4) != 0) return false;
  size_t p = 26;
  const uint32_t colorModeLen = be32(&buf[p]);
  p += 4 + colorModeLen;
  if (p + 4 > buf.size()) return false;
  const uint32_t resLen = be32(&buf[p]);
  p += 4;
  const size_t resEnd = p + resLen;
  if (resEnd > buf.size()) return false;
  while (p + 12 <= resEnd) {
    if (std::memcmp(&buf[p], "8BIM", 4) != 0) return false;
    const uint16_t id = static_cast<uint16_t>((buf[p + 4] << 8) | buf[p + 5]);
    size_t q = p + 6;
    const uint8_t nameLen = buf[q];
    q += 1 + nameLen;
    if ((1 + nameLen) % 2) q += 1;  // Pascal 名补到偶
    if (q + 4 > resEnd) return false;
    const uint32_t dlen = be32(&buf[q]);
    q += 4;
    if (q + dlen > resEnd) return false;
    if (id == 0x040F) {
      icc->assign(buf.begin() + q, buf.begin() + q + dlen);
      return true;
    }
    p = q + dlen + (dlen % 2);
  }
  return false;
}

/**
 * 独立 CRC-32（PNG 规范：多项式 0xEDB88320，反射，初值 0xFFFFFFFF，末尾取反）。
 * 逐位计算、不查表，与 icc_embed 内部实现完全无关，保证"校验是真做的"这件事可被独立验证。
 */
uint32_t testCrc32(const uint8_t* d, size_t n) {
  uint32_t c = 0xFFFFFFFFu;
  for (size_t i = 0; i < n; ++i) {
    c ^= d[i];
    for (int k = 0; k < 8; ++k) {
      c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
    }
  }
  return c ^ 0xFFFFFFFFu;
}

/** 独立 zlib 解码：只吃 stored 块（RFC 1951 §3.2.4）+ 校验 Adler-32（RFC 1950 §9）
 *  blocks/adlerOk 非空时回填实际消耗的 stored 块数与 Adler 校验结果（大 profile 用例据此断言）。 */
bool zlibInflateStored(const std::vector<uint8_t>& z, std::vector<uint8_t>* out,
                       int* blocks = nullptr, bool* adlerOk = nullptr) {
  if (blocks) *blocks = 0;
  if (adlerOk) *adlerOk = false;
  if (z.size() < 6) return false;
  const uint8_t cmf = z[0];
  const uint8_t flg = z[1];
  if ((cmf & 0x0Fu) != 8) return false;                        // CM 必须是 deflate
  if (static_cast<int>((static_cast<uint32_t>(cmf) << 8) | flg) % 31 != 0) return false;
  if ((flg & 0x20u) != 0) return false;                        // FDICT 在 PNG 中禁止
  size_t p = 2;
  int count = 0;
  out->clear();
  for (;;) {
    if (p >= z.size()) return false;
    const uint8_t hdr = z[p++];
    const int bfinal = hdr & 0x01;
    const int btype = (hdr >> 1) & 0x03;
    // 已知解析器限制（不是实现 bug）：本解析器只实现 stored 块（BTYPE=00）。
    // 若哪天 encode 侧改用真 deflate（固定/动态 Huffman，BTYPE=01/10），这里会以
    // "解析失败" 而非 "实现变更" 的形式报错 —— 届时必须先扩展本解析器，再改编码端，
    // 否则失败信息会指向错误的根因。同理 Adler-32/NLEN 校验失败也走同一条返回路径。
    if (btype != 0) return false;
    if (p + 4 > z.size()) return false;
    const uint16_t len = le16(&z[p]);
    const uint16_t nlen = le16(&z[p + 2]);
    if (static_cast<uint16_t>(~len) != nlen) return false;  // NLEN 必须是 LEN 的按位取反
    p += 4;
    if (p + len > z.size()) return false;
    out->insert(out->end(), z.begin() + static_cast<std::ptrdiff_t>(p),
                z.begin() + static_cast<std::ptrdiff_t>(p + len));
    p += len;
    ++count;
    if (bfinal) break;
  }
  if (p + 4 > z.size()) return false;
  const uint32_t expect = be32(&z[p]);
  uint32_t a = 1;
  uint32_t b = 0;
  for (size_t i = 0; i < out->size(); ++i) {
    a = (a + (*out)[i]) % 65521u;
    b = (b + a) % 65521u;
  }
  if (blocks) *blocks = count;
  if (adlerOk) *adlerOk = (expect == ((b << 16) | a));
  return expect == ((b << 16) | a);
}

/** 独立 PNG 解析：校验签名 + 逐 chunk 校验 CRC + 解 iCCP 的 zlib 流，拼出 profile 字节 */
bool pngExtractIcc(const std::string& path, std::vector<uint8_t>* icc, int* chunkCount,
                   int* zlibBlocks = nullptr, bool* adlerOk = nullptr) {
  if (zlibBlocks) *zlibBlocks = 0;
  if (adlerOk) *adlerOk = false;
  std::vector<uint8_t> buf;
  if (!readFile(path, &buf) || buf.size() < 8) return false;
  static const uint8_t kSig[8] = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
  if (std::memcmp(buf.data(), kSig, 8) != 0) return false;
  *chunkCount = 0;
  bool found = false;
  size_t p = 8;
  while (p + 12 <= buf.size()) {
    const uint32_t len = be32(&buf[p]);
    if (p + 12 + static_cast<size_t>(len) > buf.size()) return false;
    // CRC 覆盖 type+data，不覆盖 length
    const uint32_t expect = be32(&buf[p + 8 + len]);
    const uint32_t actual = testCrc32(&buf[p + 4], static_cast<size_t>(len) + 4);
    if (expect != actual) return false;
    const std::string type(reinterpret_cast<const char*>(&buf[p + 4]), 4);
    ++(*chunkCount);
    if (type == "iCCP") {
      const uint8_t* d = &buf[p + 8];
      size_t nameLen = 0;
      while (nameLen < len && d[nameLen] != 0) {
        if (d[nameLen] < 0x20 || d[nameLen] > 0x7E) return false;  // 必须 ASCII 可打印
        ++nameLen;
      }
      if (nameLen == 0 || nameLen > 79) return false;  // 规范：1..79 字节
      if (nameLen + 2 > len) return false;
      if (d[nameLen + 1] != 0) return false;  // compressionMethod 必须为 0
      const std::vector<uint8_t> z(d + nameLen + 2, d + len);
      found = zlibInflateStored(z, icc, zlibBlocks, adlerOk);
      if (!found) return false;
    }
    if (type == "IEND") break;
    p += 12 + static_cast<size_t>(len);
  }
  return found;
}

/** 读某个 chunk 的 data 长度（nullptr 语义：没找到返回 false） */
bool pngChunkDataLength(const std::string& path, const char* type, size_t* len) {
  std::vector<uint8_t> buf;
  if (!readFile(path, &buf) || buf.size() < 8) return false;
  size_t p = 8;
  while (p + 12 <= buf.size()) {
    const uint32_t l = be32(&buf[p]);
    if (p + 12 + static_cast<size_t>(l) > buf.size()) return false;
    if (std::memcmp(&buf[p + 4], type, 4) == 0) {
      *len = static_cast<size_t>(l);
      return true;
    }
    p += 12 + static_cast<size_t>(l);
  }
  return false;
}

/** 定位首个 IDAT 的 data 起始偏移（坏 CRC 用例需要精确翻一个像素数据字节） */
bool pngFirstIdatDataOffset(const std::string& path, size_t* off) {
  std::vector<uint8_t> buf;
  if (!readFile(path, &buf) || buf.size() < 8) return false;
  size_t p = 8;
  while (p + 12 <= buf.size()) {
    const uint32_t len = be32(&buf[p]);
    if (p + 12 + static_cast<size_t>(len) > buf.size()) return false;
    if (std::memcmp(&buf[p + 4], "IDAT", 4) == 0) {
      if (len == 0) return false;
      *off = p + 8;
      return true;
    }
    p += 12 + static_cast<size_t>(len);
  }
  return false;
}

std::vector<uint8_t> makeImage(int w, int h) {
  std::vector<uint8_t> px(static_cast<size_t>(w) * h * 3);
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      const size_t i = (static_cast<size_t>(y) * w + x) * 3;
      px[i + 0] = static_cast<uint8_t>(x * 255 / (w - 1));
      px[i + 1] = static_cast<uint8_t>(y * 255 / (h - 1));
      px[i + 2] = static_cast<uint8_t>((x + y) * 255 / (w + h - 2));
    }
  }
  return px;
}

/** 大 profile 用例的结果（并入 m4-embed-report.json 顶层的 "largeProfile" 对象） */
struct LargeProfileResult {
  size_t bytes = 0;          // 解回的 profile 字节数
  int storedBlocks = 0;      // 实际跨过的 stored 块数
  bool byteEqual = false;    // 与写入值逐字节一致
  bool adlerOk = false;      // zlib 尾部 Adler-32 校验通过
  size_t iccpChunkBytes = 0; // iCCP chunk 的 data 长度
};

/**
 * 大 profile 多 stored 块回归：140000 B → 140000/65535 向上取整 = 3 块
 * （65535 + 65535 + 8930），专门覆盖 zlibStoreRaw 里 do/while 循环体、
 * LEN/NLEN 计算、非末块 BFINAL=0 这几个单块用例永远走不到的分支。
 * 伪 profile 用可逐字节复核的模式串（非全 0，避免"解码出全 0 也算过"的假通过）。
 */
LargeProfileResult testLargeProfilePng(int w, int h) {
  LargeProfileResult r;
  const size_t kProfileBytes = 140000;
  const std::vector<uint8_t> px = makeImage(w, h);
  lumina::imaging::Image img;
  img.width = w;
  img.height = h;
  img.pixels = px;

  std::vector<uint8_t> big(kProfileBytes);
  for (size_t i = 0; i < big.size(); ++i) {
    big[i] = static_cast<uint8_t>((i * 37 + 11) & 0xFF);
  }

  const std::string path = "m4-embed-large.png";
  std::string err;
  bool ok = lumina::imaging::encodePng(path, img, &err);
  check(ok, "大 profile PNG 编码落盘 %s", err.c_str());
  ok = ok && lumina::icc_embed::injectPngIcc(path, big, &err);
  check(ok, "大 profile（%zu B）注入 iCCP %s", kProfileBytes, err.c_str());

  bool has = false;
  std::string herr;
  const bool scanned = lumina::icc_embed::pngHasIcc(path, &has, &herr);
  check(scanned && has, "大 profile PNG pngHasIcc=true %s", herr.c_str());

  std::vector<uint8_t> got;
  int chunkCount = 0;
  int blocks = 0;
  bool adlerOk = false;
  const bool parsed = ok && pngExtractIcc(path, &got, &chunkCount, &blocks, &adlerOk);
  check(parsed, "大 profile 独立解析出 iCCP（%d chunk，解 zlib 后 %zu 字节）", chunkCount,
        got.size());
  check(parsed && got == big, "大 profile 逐字节一致（期望 %zu，实得 %zu）", kProfileBytes,
        got.size());
  check(parsed && adlerOk, "大 profile Adler-32 校验通过");

  const int expectBlocks = static_cast<int>((kProfileBytes + 65534) / 65535);
  check(blocks == expectBlocks, "大 profile 跨 %d 个 stored 块（140000/65535 向上取整，实得 %d）",
        expectBlocks, blocks);

  // iCCP data 长度算式：profileName(18) + NUL(1) + compressionMethod(1)
  //   + zlib 头(2) + 每块头(BFINAL/BTYPE 1 + LEN 2 + NLEN 2 = 5) × 块数 + profile(140000) + Adler(4)
  size_t iccpLen = 0;
  const bool gotLen = pngChunkDataLength(path, "iCCP", &iccpLen);
  const size_t expectIc = std::strlen("LUMINA RGB Profile") + 1 + 1 + 2 +
                          static_cast<size_t>(expectBlocks) * 5 + kProfileBytes + 4;
  check(gotLen && iccpLen == expectIc,
        "大 profile iCCP data 长度 = %zu（算式 18+1+1+2+3×5+140000+4，实得 %zu）", expectIc,
        iccpLen);

  r.bytes = got.size();
  r.storedBlocks = blocks;
  r.byteEqual = parsed && got == big;
  r.adlerOk = adlerOk;
  r.iccpChunkBytes = iccpLen;
  return r;
}

void appendBe32(std::vector<uint8_t>* v, uint32_t x) {
  v->push_back(static_cast<uint8_t>((x >> 24) & 0xFFu));
  v->push_back(static_cast<uint8_t>((x >> 16) & 0xFFu));
  v->push_back(static_cast<uint8_t>((x >> 8) & 0xFFu));
  v->push_back(static_cast<uint8_t>(x & 0xFFu));
}

/** 手工在 IHDR 之后插入一个 sRGB chunk（data = 1 字节 rendering intent），
 *  CRC 用本文件的独立 testCrc32 计算 —— 用来构造"已含 sRGB chunk 的合法 PNG"。 */
bool pngInsertSrgbChunk(const std::string& src, const std::string& dst) {
  std::vector<uint8_t> buf;
  if (!readFile(src, &buf) || buf.size() < 8) return false;
  static const uint8_t kSig[8] = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
  if (std::memcmp(buf.data(), kSig, 8) != 0) return false;
  const uint32_t ihdrLen = be32(&buf[8]);
  const size_t insertAt = 8 + 12 + static_cast<size_t>(ihdrLen);
  if (insertAt > buf.size()) return false;
  static const char kType[4] = {'s', 'R', 'G', 'B'};
  const uint8_t intent = 0;  // rendering intent = perceptual
  std::vector<uint8_t> typeAndData(kType, kType + 4);
  typeAndData.push_back(intent);
  std::vector<uint8_t> chunk;
  appendBe32(&chunk, 1);                                   // data 长度 = 1 字节
  chunk.insert(chunk.end(), kType, kType + 4);             // type
  chunk.push_back(intent);                                 // data
  appendBe32(&chunk, testCrc32(typeAndData.data(), typeAndData.size()));  // CRC 覆盖 type+data
  std::vector<uint8_t> out(buf.begin(),
                           buf.begin() + static_cast<std::ptrdiff_t>(insertAt));
  out.insert(out.end(), chunk.begin(), chunk.end());
  out.insert(out.end(), buf.begin() + static_cast<std::ptrdiff_t>(insertAt), buf.end());
  FILE* f = std::fopen(dst.c_str(), "wb");
  if (!f) return false;
  const bool ok = std::fwrite(out.data(), 1, out.size(), f) == out.size();
  std::fclose(f);
  return ok;
}

/**
 * sRGB 互斥 fail-closed 契约（PNG 规范 11.3.3.5：iCCP 与 sRGB chunk 不得同时存在）。
 * 构造一份含 sRGB chunk 的合法 PNG，断言：
 *  1) 它仍能被 pngHasIcc 正常扫描（签名/CRC 都对）且 has=false；
 *  2) injectPngIcc 拒绝写入，错误串含 "sRGB"；
 *  3) 被拒绝后原文件逐字节未被改动（用独立 CRC-32 比对整文件）。
 */
bool testSrgbMutexReject(int w, int h) {
  const std::string base = "m4-embed-srgbchunk-base.png";
  const std::string target = "m4-embed-srgbchunk.png";
  const std::vector<uint8_t> px = makeImage(w, h);
  lumina::imaging::Image img;
  img.width = w;
  img.height = h;
  img.pixels = px;

  std::string err;
  bool ok = lumina::imaging::encodePng(base, img, &err);
  check(ok, "sRGB 互斥：基础 PNG 编码落盘 %s", err.c_str());
  ok = ok && pngInsertSrgbChunk(base, target);
  check(ok, "sRGB 互斥：手工插入 sRGB chunk（CRC 用独立实现算）成功");

  std::vector<uint8_t> icc;
  if (ok && !lumina::color::profileBytes(lumina::color::ProfileId::Srgb, &icc, &err)) {
    check(false, "sRGB 互斥：profile 生成失败 %s", err.c_str());
    return false;
  }

  // 1) 含 sRGB 的文件本身合法 + 没有 iCCP
  bool has = true;
  std::string herr;
  const bool scanned = ok && lumina::icc_embed::pngHasIcc(target, &has, &herr);
  check(scanned && !has, "sRGB 互斥：含 sRGB 的 PNG 可正常扫描且 pngHasIcc=false %s",
        herr.c_str());

  // 2) 注入必须被拒绝，且原因点明 sRGB
  std::vector<uint8_t> before;
  const bool readBefore = readFile(target, &before);
  const uint32_t beforeCrc = before.empty() ? 0u : testCrc32(before.data(), before.size());
  std::string ierr;
  const bool injected = ok && lumina::icc_embed::injectPngIcc(target, icc, &ierr);
  // 断言真实关键词（不是"错误串非空"这种空断言）：必须点明 sRGB 且明确是"拒绝"
  check(ok && !injected && ierr.find("sRGB") != std::string::npos &&
            ierr.find("拒绝") != std::string::npos,
        "sRGB 互斥：已含 sRGB chunk 时注入被拒绝（%s）", ierr.c_str());

  // 3) 拒绝发生在写盘之前 —— 原文件必须逐字节不变
  std::vector<uint8_t> after;
  const bool readAfter = readFile(target, &after);
  const uint32_t afterCrc = after.empty() ? 0u : testCrc32(after.data(), after.size());
  check(readBefore && readAfter && beforeCrc == afterCrc,
        "sRGB 互斥：被拒绝后原文件未被改动（整文件 CRC 0x%08X）", afterCrc);
  return ok && !injected;
}

bool writeBytesFile(const std::string& path, const std::vector<uint8_t>& data) {
  FILE* f = std::fopen(path.c_str(), "wb");
  if (!f) return false;
  const bool ok = std::fwrite(data.data(), 1, data.size(), f) == data.size();
  std::fclose(f);
  return ok;
}

/** 读 ICC 头 [24:36] 的 dateTimeNumber（6 个 uint16 大端）→ "YYYY-MM-DDTHH:MM:SS" */
std::string iccCreatedString(const std::vector<uint8_t>& icc) {
  if (icc.size() < 36) return std::string("(头过短)");
  unsigned f[6] = {0, 0, 0, 0, 0, 0};
  for (size_t i = 0; i < 6; ++i) {
    f[i] = (static_cast<unsigned>(icc[24 + i * 2]) << 8) | icc[24 + i * 2 + 1];
  }
  char buf[40] = {0};
  std::snprintf(buf, sizeof(buf), "%04u-%02u-%02uT%02u:%02u:%02u", f[0], f[1], f[2], f[3], f[4],
                f[5]);
  return std::string(buf);
}

/** profile 序列化确定性（并入报告 "profileDeterminism"） */
struct ProfileDeterminismResult {
  bool srgbEqual = false;
  bool adobeRgbEqual = false;
  bool createdOk = false;   // [24:36] 已归一化为 2000-01-01T00:00:00（两个空间都核对）
  size_t srgbBytes = 0;
  size_t adobeBytes = 0;
  std::string created;      // 实测字符串，写进报告
};

/**
 * profile 字节确定性回归：同一进程内连续两次 profileBytes 必须逐字节相等，
 * 且 ICC 头 [24:36] 的 dateTimeNumber 已归一化为固定值 2000-01-01T00:00:00。
 * 否则同一张照片今天/明天导出，嵌进 JPG/TIFF/PSD/PNG 的 profile 不同，
 * 导出物不可位级复现，golden-hash 回归当场失效。
 * 附带产物 m4-profile-srgb-a.icc / -b.icc，供跨进程比对（不要删）。
 */
ProfileDeterminismResult testProfileDeterminism() {
  ProfileDeterminismResult r;
  std::string err;

  // ---- sRGB ----
  std::vector<uint8_t> a;
  std::vector<uint8_t> b;
  const bool okA = lumina::color::profileBytes(lumina::color::ProfileId::Srgb, &a, &err);
  const bool okB = lumina::color::profileBytes(lumina::color::ProfileId::Srgb, &b, &err);
  check(okA && okB, "确定性：sRGB profile 连续两次序列化成功 %s", err.c_str());
  r.srgbEqual = okA && okB && a == b && !a.empty();
  check(r.srgbEqual, "确定性：sRGB 连续两次字节完全一致（%zu 字节）", a.size());
  r.created = iccCreatedString(a);
  const bool srgbCreatedOk = (r.created == "2000-01-01T00:00:00");
  check(srgbCreatedOk, "确定性：sRGB 头 [24:36] = 固定时间 2000-01-01T00:00:00（实际 %s）",
        r.created.c_str());
  const bool wroteA = writeBytesFile("m4-profile-srgb-a.icc", a);
  const bool wroteB = writeBytesFile("m4-profile-srgb-b.icc", b);
  check(wroteA && wroteB, "确定性：已写 m4-profile-srgb-a.icc / -b.icc 供跨进程比对");

  // ---- AdobeRGB ----
  std::vector<uint8_t> c;
  std::vector<uint8_t> d;
  const bool okC = lumina::color::profileBytes(lumina::color::ProfileId::AdobeRgb, &c, &err);
  const bool okD = lumina::color::profileBytes(lumina::color::ProfileId::AdobeRgb, &d, &err);
  check(okC && okD, "确定性：AdobeRGB profile 连续两次序列化成功 %s", err.c_str());
  r.adobeRgbEqual = okC && okD && c == d && !c.empty();
  check(r.adobeRgbEqual, "确定性：AdobeRGB 连续两次字节完全一致（%zu 字节）", c.size());
  const std::string createdAdobe = iccCreatedString(c);
  const bool adobeCreatedOk = (createdAdobe == "2000-01-01T00:00:00");
  check(adobeCreatedOk, "确定性：AdobeRGB 头 [24:36] = 固定时间（实际 %s）",
        createdAdobe.c_str());

  r.createdOk = srgbCreatedOk && adobeCreatedOk;
  r.srgbBytes = a.size();
  r.adobeBytes = c.size();
  return r;
}

}  // namespace

int main() {
  std::printf("== M4-02 导出嵌入 ICC ==\n");

  const int w = 32, h = 24;
  const std::vector<uint8_t> pixels = makeImage(w, h);
  lumina::imaging::Image img;
  img.width = w;
  img.height = h;
  img.pixels = pixels;

  struct Row {
    std::string format;
    std::string space;
    std::string file;
    size_t iccBytes;
    bool byteEqual;
    std::string desc;
    std::string colorSig;
  };
  std::vector<Row> rows;

  const struct {
    lumina::color::ProfileId id;
    const char* name;
    const char* space;
  } spaces[2] = {{lumina::color::ProfileId::Srgb, "sRGB IEC61966-2.1", "sRGB"},
                 {lumina::color::ProfileId::AdobeRgb, "AdobeRGB (1998)", "AdobeRGB"}};

  for (const auto& sp : spaces) {
    std::vector<uint8_t> icc;
    std::string err;
    if (!lumina::color::profileBytes(sp.id, &icc, &err)) {
      check(false, "%s profile 生成失败: %s", sp.name, err.c_str());
      continue;
    }

    // ---- JPEG ----
    {
      const std::string path = std::string("m4-embed-") + sp.space + ".jpg";
      std::string jerr;
      bool ok = lumina::core::jpeg::encodeToFile(path, img, 92, &jerr);
      ok = ok && lumina::icc_embed::injectJpegIcc(path, icc, &jerr);
      check(ok, "%s JPEG 写入并注入 ICC %s", sp.name, jerr.c_str());
      std::vector<uint8_t> got;
      int segs = 0;
      const bool parsed = jpegExtractIcc(path, &got, &segs);
      check(parsed, "%s JPEG 独立解析出 APP2 ICC（%d 段）", sp.name, segs);
      if (parsed) {
        rows.push_back({std::string("jpg"), sp.space, path, got.size(), got == icc,
                        iccDescription(got),
                        std::string(reinterpret_cast<const char*>(&got[16]), 4)});
      }
      // 重复注入必须失败（不静默写两份）
      std::string dupErr;
      check(!lumina::icc_embed::injectJpegIcc(path, icc, &dupErr), "%s 重复注入被拒绝", sp.name);
    }

    // ---- TIFF ----
    {
      const std::string path = std::string("m4-embed-") + sp.space + ".tif";
      std::string terr;
      const bool ok = lumina::exporter::Exporter::writeTiffForTest(path, img, icc, &terr);
      check(ok, "%s TIFF 写入并带 ICC %s", sp.name, terr.c_str());
      std::vector<uint8_t> got;
      const bool parsed = ok && tiffExtractIcc(path, &got);
      check(parsed, "%s TIFF 独立解析出 tag 34675", sp.name);
      if (parsed) {
        rows.push_back({std::string("tiff"), sp.space, path, got.size(), got == icc,
                        iccDescription(got),
                        std::string(reinterpret_cast<const char*>(&got[16]), 4)});
      }
    }

    // ---- PSD ----
    {
      const std::string path = std::string("m4-embed-") + sp.space + ".psd";
      lumina::psd::WriteOptions opts;
      opts.width = w;
      opts.height = h;
      opts.depth = 8;
      opts.rle = true;
      opts.writeComposite = true;
      opts.iccProfile = icc;
      lumina::psd::Document doc;
      lumina::psd::Layer layer;
      layer.name = "合成";
      layer.width = w;
      layer.height = h;
      layer.rgb = pixels;
      layer.opacity = 255;
      layer.blendKey = "norm";
      layer.visible = true;
      doc.layers.push_back(layer);
      doc.composite = pixels;
      std::string perr;
      const bool ok = lumina::psd::writePsd(path, doc, opts, &perr);
      check(ok, "%s PSD 写入并带资源 0x040F %s", sp.name, perr.c_str());
      std::vector<uint8_t> got;
      const bool parsed = ok && psdExtractIcc(path, &got);
      check(parsed, "%s PSD 独立解析出 0x040F", sp.name);
      if (parsed) {
        rows.push_back({std::string("psd"), sp.space, path, got.size(), got == icc,
                        iccDescription(got),
                        std::string(reinterpret_cast<const char*>(&got[16]), 4)});
      }
    }

    // ---- PNG（iCCP，zlib stored 流） ----
    {
      const std::string path = std::string("m4-embed-") + sp.space + ".png";
      std::string perr;
      bool ok = lumina::imaging::encodePng(path, img, &perr);
      check(ok, "%s PNG 编码落盘 %s", sp.name, perr.c_str());

      // 注入前：pngHasIcc 必须为 false，否则下面的断言就没有意义
      bool before = true;
      std::string herrBefore;
      const bool scanBefore = lumina::icc_embed::pngHasIcc(path, &before, &herrBefore);
      check(scanBefore && !before, "%s PNG 注入前 pngHasIcc=false %s", sp.name,
            herrBefore.c_str());

      ok = ok && lumina::icc_embed::injectPngIcc(path, icc, &perr);
      check(ok, "%s PNG 注入 iCCP %s", sp.name, perr.c_str());

      // 注入后：pngHasIcc 必须为 true
      bool after = false;
      std::string herrAfter;
      const bool scanAfter = lumina::icc_embed::pngHasIcc(path, &after, &herrAfter);
      check(scanAfter && after, "%s PNG 注入后 pngHasIcc=true %s", sp.name, herrAfter.c_str());

      std::vector<uint8_t> got;
      int chunkCount = 0;
      const bool parsed = ok && pngExtractIcc(path, &got, &chunkCount);
      check(parsed, "%s PNG 独立解析出 iCCP（共 %d chunk，解 zlib 后 %zu 字节）", sp.name,
            chunkCount, got.size());
      if (parsed) {
        rows.push_back({std::string("png"), sp.space, path, got.size(), got == icc,
                        iccDescription(got),
                        std::string(reinterpret_cast<const char*>(&got[16]), 4)});
      }

      // 重复注入必须失败（不静默写两份 iCCP）
      // 注意：必须先算布尔值再调用 check()。若写成
      //   check(!injectPngIcc(path, icc, &dupErr), "…（%s）", sp.name, dupErr.c_str())
      // 则函数实参求值顺序未指定（C++ [expr.call]/8）：dupErr.c_str() 可能在
      // injectPngIcc 之前求值（此时 dupErr 为空，SSO 缓冲），随后赋值触发扩容，
      // 打印时读到的就是已释放内存 —— 实测打印出 "（T）"/"（X）" 这种单字符乱码。
      std::string dupErr;
      const bool dupRejected = !lumina::icc_embed::injectPngIcc(path, icc, &dupErr);
      check(dupRejected, "%s PNG 重复注入被拒绝（%s）", sp.name, dupErr.c_str());

      // 坏 CRC 必须被抓住：复制一份，翻转 IDAT 里一个字节，pngHasIcc 必须报 CRC 错误
      {
        std::vector<uint8_t> src;
        size_t idatOff = 0;
        const bool readable = readFile(path, &src) && pngFirstIdatDataOffset(path, &idatOff) &&
                              idatOff < src.size();
        const std::string corrupt = path + ".badcrc.png";
        bool written = false;
        if (readable) {
          src[idatOff] ^= 0xFF;
          FILE* cf = std::fopen(corrupt.c_str(), "wb");
          if (cf) {
            written = std::fwrite(src.data(), 1, src.size(), cf) == src.size();
            std::fclose(cf);
          }
        }
        bool hasBad = true;
        std::string badErr;
        const bool scanned = written && lumina::icc_embed::pngHasIcc(corrupt, &hasBad, &badErr);
        check(written && !scanned && !hasBad && badErr.find("CRC") != std::string::npos,
              "%s PNG 坏 CRC 被检出（%s）", sp.name, badErr.c_str());
      }
    }
  }

  // ---- 大 profile：覆盖 stored 多块路径（do/while 循环体、非末块 BFINAL=0、LEN/NLEN） ----
  const LargeProfileResult large = testLargeProfilePng(w, h);

  // ---- sRGB 互斥 fail-closed：覆盖 injectPngIcc 里"已含 sRGB chunk 则拒绝"分支 ----
  const bool srgbRejected = testSrgbMutexReject(w, h);

  // ---- profile 字节确定性：ICC 头创建时间已归一化，导出物可位级复现 ----
  const ProfileDeterminismResult det = testProfileDeterminism();

  for (const auto& r : rows) {
    check(r.byteEqual, "%s/%s profile 字节一致（%zu 字节，描述 \"%s\"，色空间 %s）",
          r.format.c_str(), r.space.c_str(), r.iccBytes, r.desc.c_str(), r.colorSig.c_str());
    check(r.colorSig == "RGB ", "%s/%s 色空间签名为 RGB", r.format.c_str(), r.space.c_str());
  }
  check(rows.size() == 8, "八个文件全部产出并可解析（2 空间 × 4 格式，实际 %zu）", rows.size());
  check(large.byteEqual && large.storedBlocks == 3 && large.adlerOk && srgbRejected,
        "扩展用例全部通过（大 profile 140000 B / 3 块 / Adler 通过 + sRGB 互斥拒绝）");
  check(det.srgbEqual && det.adobeRgbEqual && det.createdOk,
        "确定性：两空间 profile 字节稳定且创建时间已归一化（sRGB %zu B / AdobeRGB %zu B）",
        det.srgbBytes, det.adobeBytes);

  FILE* f = std::fopen("m4-embed-report.json", "wb");
  if (!f) {
    std::printf("  \xE2\x9C\x97 无法写 m4-embed-report.json\n");
    ++g_fail;
  } else {
    std::fprintf(f, "{\n  \"task\": \"M4-02\",\n  \"note\": \"profile 由内置构造（lcms2），非分发第三方商业 profile\",\n");
    std::fprintf(f, "  \"files\": [\n");
    for (size_t i = 0; i < rows.size(); ++i) {
      std::fprintf(f,
                   "    {\"format\": \"%s\", \"space\": \"%s\", \"file\": \"%s\", \"iccBytes\": %zu, "
                   "\"byteEqual\": %s, \"description\": \"%s\", \"colorSpaceSig\": \"%s\"}%s\n",
                   rows[i].format.c_str(), rows[i].space.c_str(), rows[i].file.c_str(),
                   rows[i].iccBytes, rows[i].byteEqual ? "true" : "false", rows[i].desc.c_str(),
                   rows[i].colorSig.c_str(), (i + 1 < rows.size()) ? "," : "");
    }
    // 大 profile（多 stored 块）单独一项，不改上面 files 数组的字段名
    std::fprintf(f,
                 "  ],\n  \"largeProfile\": {\"bytes\": %zu, \"storedBlocks\": %d, "
                 "\"byteEqual\": %s, \"adlerOk\": %s, \"iccpChunkBytes\": %zu},\n"
                 "  \"profileDeterminism\": {\"srgbEqual\": %s, \"adobeRgbEqual\": %s, "
                 "\"created\": \"%s\", \"srgbBytes\": %zu, \"adobeRgbBytes\": %zu},\n"
                 "  \"pass\": %s\n}\n",
                 large.bytes, large.storedBlocks, large.byteEqual ? "true" : "false",
                 large.adlerOk ? "true" : "false", large.iccpChunkBytes,
                 det.srgbEqual ? "true" : "false", det.adobeRgbEqual ? "true" : "false",
                 det.created.c_str(), det.srgbBytes, det.adobeBytes,
                 g_fail == 0 ? "true" : "false");
    std::fclose(f);
    std::printf("  · 报告已写 m4-embed-report.json\n");
  }

  std::printf(g_fail == 0 ? "M4-02 PASS\n" : "M4-02 FAIL (%d)\n", g_fail);
  return g_fail == 0 ? 0 : 1;
}
