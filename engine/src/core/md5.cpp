// MD5 实现（RFC 1321 直译，小端字节序）
#include "core/md5.h"

#include <cstdio>
#include <cstring>

namespace lumina {
namespace {

constexpr uint32_t kK[64] = {
    0xd76aa478u, 0xe8c7b756u, 0x242070dbu, 0xc1bdceeeu, 0xf57c0fafu, 0x4787c62au, 0xa8304613u,
    0xfd469501u, 0x698098d8u, 0x8b44f7afu, 0xffff5bb1u, 0x895cd7beu, 0x6b901122u, 0xfd987193u,
    0xa679438eu, 0x49b40821u, 0xf61e2562u, 0xc040b340u, 0x265e5a51u, 0xe9b6c7aau, 0xd62f105du,
    0x02441453u, 0xd8a1e681u, 0xe7d3fbc8u, 0x21e1cde6u, 0xc33707d6u, 0xf4d50d87u, 0x455a14edu,
    0xa9e3e905u, 0xfcefa3f8u, 0x676f02d9u, 0x8d2a4c8au, 0xfffa3942u, 0x8771f681u, 0x6d9d6122u,
    0xfde5380cu, 0xa4beea44u, 0x4bdecfa9u, 0xf6bb4b60u, 0xbebfbc70u, 0x289b7ec6u, 0xeaa127fau,
    0xd4ef3085u, 0x04881d05u, 0xd9d4d039u, 0xe6db99e5u, 0x1fa27cf8u, 0xc4ac5665u, 0xf4292244u,
    0x432aff97u, 0xab9423a7u, 0xfc93a039u, 0x655b59c3u, 0x8f0ccc92u, 0xffeff47du, 0x85845dd1u,
    0x6fa87e4fu, 0xfe2ce6e0u, 0xa3014314u, 0x4e0811a1u, 0xf7537e82u, 0xbd3af235u, 0x2ad7d2bbu,
    0xeb86d391u};

constexpr uint32_t kShift[64] = {
    7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
    5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20,
    4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
    6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21};

inline uint32_t rotateLeft(uint32_t x, uint32_t n) { return (x << n) | (x >> (32 - n)); }

inline uint32_t funcF(uint32_t x, uint32_t y, uint32_t z) { return (x & y) | (~x & z); }
inline uint32_t funcG(uint32_t x, uint32_t y, uint32_t z) { return (x & z) | (y & ~z); }
inline uint32_t funcH(uint32_t x, uint32_t y, uint32_t z) { return x ^ y ^ z; }
inline uint32_t funcI(uint32_t x, uint32_t y, uint32_t z) { return y ^ (x | ~z); }

void toHex(const uint8_t* digest, std::string* out) {
  static const char* kHex = "0123456789abcdef";
  out->clear();
  out->reserve(32);
  for (int i = 0; i < 16; ++i) {
    out->push_back(kHex[(digest[i] >> 4) & 0xF]);
    out->push_back(kHex[digest[i] & 0xF]);
  }
}

}  // namespace

Md5Stream::Md5Stream() : count_(0), bufferLen_(0) {
  state_[0] = 0x67452301u;
  state_[1] = 0xefcdab89u;
  state_[2] = 0x98badcfeu;
  state_[3] = 0x10325476u;
  std::memset(buffer_, 0, sizeof(buffer_));
}

void Md5Stream::transform(const uint8_t block[64]) {
  uint32_t m[16];
  for (int i = 0; i < 16; ++i) {
    m[i] = static_cast<uint32_t>(block[i * 4]) | (static_cast<uint32_t>(block[i * 4 + 1]) << 8) |
           (static_cast<uint32_t>(block[i * 4 + 2]) << 16) |
           (static_cast<uint32_t>(block[i * 4 + 3]) << 24);
  }
  uint32_t a = state_[0], b = state_[1], c = state_[2], d = state_[3];
  for (int i = 0; i < 64; ++i) {
    uint32_t f = 0;
    uint32_t g = 0;
    if (i < 16) { f = funcF(b, c, d); g = i; }
    else if (i < 32) { f = funcG(b, c, d); g = (5 * i + 1) % 16; }
    else if (i < 48) { f = funcH(b, c, d); g = (3 * i + 5) % 16; }
    else { f = funcI(b, c, d); g = (7 * i) % 16; }
    uint32_t tmp = d;
    d = c;
    c = b;
    b = b + rotateLeft(a + f + kK[i] + m[g], kShift[i]);
    a = tmp;
  }
  state_[0] += a;
  state_[1] += b;
  state_[2] += c;
  state_[3] += d;
}

void Md5Stream::update(const void* data, size_t size) {
  const uint8_t* p = static_cast<const uint8_t*>(data);
  count_ += size;
  while (size > 0) {
    if (bufferLen_ == 0 && size >= 64) {
      transform(p);
      p += 64;
      size -= 64;
      continue;
    }
    size_t need = 64 - bufferLen_;
    size_t take = size < need ? size : need;
    std::memcpy(buffer_ + bufferLen_, p, take);
    bufferLen_ += take;
    p += take;
    size -= take;
    if (bufferLen_ == 64) {
      transform(buffer_);
      bufferLen_ = 0;
    }
  }
}

std::string Md5Stream::hexDigest() {
  uint8_t tail[128];
  uint64_t bits = count_ * 8;
  size_t padLen = (count_ % 64) < 56 ? (56 - count_ % 64) : (120 - count_ % 64);
  std::memset(tail, 0, sizeof(tail));
  tail[0] = 0x80;
  for (int i = 0; i < 8; ++i) {
    tail[padLen + i] = static_cast<uint8_t>((bits >> (8 * i)) & 0xFF);
  }
  update(tail, padLen + 8);
  uint8_t digest[16];
  for (int i = 0; i < 4; ++i) {
    digest[i * 4] = static_cast<uint8_t>(state_[i] & 0xFF);
    digest[i * 4 + 1] = static_cast<uint8_t>((state_[i] >> 8) & 0xFF);
    digest[i * 4 + 2] = static_cast<uint8_t>((state_[i] >> 16) & 0xFF);
    digest[i * 4 + 3] = static_cast<uint8_t>((state_[i] >> 24) & 0xFF);
  }
  std::string out;
  toHex(digest, &out);
  return out;
}

std::string md5OfBuffer(const void* data, size_t size) {
  Md5Stream stream;
  stream.update(data, size);
  return stream.hexDigest();
}

std::string md5OfFile(const std::string& path, std::string* err) {
  FILE* f = fopen(path.c_str(), "rb");
  if (!f) {
    if (err) *err = "open failed: " + path;
    return std::string();
  }
  Md5Stream stream;
  char buf[65536];
  size_t n = 0;
  while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
    stream.update(buf, n);
  }
  bool bad = ferror(f) != 0;
  fclose(f);
  if (bad) {
    if (err) *err = "read failed: " + path;
    return std::string();
  }
  return stream.hexDigest();
}

}  // namespace lumina
