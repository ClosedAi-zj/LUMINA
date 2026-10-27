// M4-02 回归：导出嵌入 ICC（JPEG APP2 / TIFF tag 34675 / PSD resource 0x040F）
// 判据：第三方解析（本文件内独立实现，不复用 writer 路径）读出的 profile 字节与写入值
//       **逐字节一致**，色彩空间签名正确，描述串正确。
// 产物：m4-embed-report.json
#include <algorithm>
#include <cstdarg>
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
  }

  for (const auto& r : rows) {
    check(r.byteEqual, "%s/%s profile 字节一致（%zu 字节，描述 \"%s\"，色空间 %s）",
          r.format.c_str(), r.space.c_str(), r.iccBytes, r.desc.c_str(), r.colorSig.c_str());
    check(r.colorSig == "RGB ", "%s/%s 色空间签名为 RGB", r.format.c_str(), r.space.c_str());
  }
  check(rows.size() == 6, "六个文件全部产出并可解析（实际 %zu）", rows.size());

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
    std::fprintf(f, "  ],\n  \"pass\": %s\n}\n", g_fail == 0 ? "true" : "false");
    std::fclose(f);
    std::printf("  · 报告已写 m4-embed-report.json\n");
  }

  std::printf(g_fail == 0 ? "M4-02 PASS\n" : "M4-02 FAIL (%d)\n", g_fail);
  return g_fail == 0 ? 0 : 1;
}
