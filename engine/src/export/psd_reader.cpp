// M3-01b / M3-03：PSD 解析实现（口径见 psd_reader.h 文件头）
#include "export/psd_reader.h"

#include <cstdio>
#include <cstring>

#include "core/logger.h"

namespace lumina {
namespace psd {
namespace {

struct Cursor {
  const uint8_t* p = nullptr;
  size_t n = 0;
  size_t pos = 0;
  bool need(size_t k) const { return pos + k <= n; }
  uint8_t u8() { return p[pos++]; }
  uint16_t u16() {
    const uint16_t v = static_cast<uint16_t>((p[pos] << 8) | p[pos + 1]);
    pos += 2;
    return v;
  }
  uint32_t u32() {
    const uint32_t v = (static_cast<uint32_t>(p[pos]) << 24) |
                       (static_cast<uint32_t>(p[pos + 1]) << 16) |
                       (static_cast<uint32_t>(p[pos + 2]) << 8) |
                       static_cast<uint32_t>(p[pos + 3]);
    pos += 4;
    return v;
  }
  int16_t i16() { return static_cast<int16_t>(u16()); }
  int32_t i32() { return static_cast<int32_t>(u32()); }
  void skip(size_t k) { pos += k; }
  std::string str(size_t k) {
    std::string s(reinterpret_cast<const char*>(p + pos), k);
    pos += k;
    return s;
  }
};

uint32_t round2(uint32_t v) { return (v + 1u) & ~1u; }
uint32_t round4(uint32_t v) { return (v + 3u) & ~3u; }

/** 读一个通道数据块：compression + [行计数表] + 数据 → 解压为原始平面 */
bool readChannel(Cursor& c, int width, int height, int bytesPerSample,
                 std::vector<uint8_t>* planeOut, std::string* err) {
  if (!c.need(2)) {
    if (err) *err = "通道数据缺少 compression 字段";
    return false;
  }
  const uint16_t compression = c.u16();
  const size_t planeBytes = static_cast<size_t>(width) * height * bytesPerSample;
  std::vector<uint8_t> plane(planeBytes, 0);
  if (compression == 0) {
    if (!c.need(planeBytes)) {
      if (err) *err = "Raw 通道数据长度不足";
      return false;
    }
    std::memcpy(plane.data(), c.p + c.pos, planeBytes);
    c.skip(planeBytes);
  } else if (compression == 1) {
    std::vector<uint16_t> rowCounts(static_cast<size_t>(height));
    if (!c.need(static_cast<size_t>(height) * 2)) {
      if (err) *err = "RLE 行计数表长度不足";
      return false;
    }
    size_t total = 0;
    for (int y = 0; y < height; ++y) {
      rowCounts[y] = c.u16();
      total += rowCounts[y];
    }
    if (!c.need(total)) {
      if (err) *err = "RLE 通道数据长度不足";
      return false;
    }
    const int rowBytes = width * bytesPerSample;
    size_t at = c.pos;
    for (int y = 0; y < height; ++y) {
      std::vector<uint8_t> row;
      if (!packBitsDecode(c.p + at, rowCounts[y], &row, rowBytes)) {
        if (err) *err = "RLE 行解码失败";
        return false;
      }
      if (row.size() != static_cast<size_t>(rowBytes)) {
        if (err) *err = "RLE 行解码长度与行宽不符";
        return false;
      }
      std::memcpy(plane.data() + static_cast<size_t>(y) * rowBytes, row.data(), rowBytes);
      at += rowCounts[y];
    }
    c.skip(total);
  } else {
    if (err) *err = "不支持的 PSD 通道压缩方式: " + std::to_string(compression);
    return false;
  }
  *planeOut = std::move(plane);
  return true;
}

/** 平面 → 交错（RGB），16bit 每样本 2 字节原样搬 */
void interleave(const std::vector<uint8_t>& r, const std::vector<uint8_t>& g,
                const std::vector<uint8_t>& b, int w, int h, int bps,
                std::vector<uint8_t>* out) {
  out->assign(static_cast<size_t>(w) * h * 3 * bps, 0);
  const size_t px = static_cast<size_t>(w) * h;
  for (size_t i = 0; i < px; ++i) {
    std::memcpy(out->data() + i * 3 * bps, r.data() + i * bps, bps);
    std::memcpy(out->data() + i * 3 * bps + bps, g.data() + i * bps, bps);
    std::memcpy(out->data() + i * 3 * bps + 2 * bps, b.data() + i * bps, bps);
  }
}

std::string blendNameFromKey(const std::string& key) {
  if (key == "norm") return "normal";
  if (key == "mul ") return "multiply";
  if (key == "scrn") return "screen";
  if (key == "over") return "overlay";
  if (key == "sLit") return "softLight";
  if (key == "hLit") return "hardLight";
  if (key == "lite") return "lighten";
  if (key == "dark") return "darken";
  if (key == "lum ") return "luminosity";
  if (key == "hue ") return "color";
  return key;  // 未收录：原样返回，调用方当未知处理（不静默改值）
}

}  // namespace

bool readPsd(const std::string& path, PsdInfo* out, std::string* err) {
  std::FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) {
    if (err) *err = "无法打开 PSD: " + path;
    return false;
  }
  std::fseek(f, 0, SEEK_END);
  const long fileSize = std::ftell(f);
  std::fseek(f, 0, SEEK_SET);
  if (fileSize < 26) {
    std::fclose(f);
    if (err) *err = "文件过小，不是合法 PSD";
    return false;
  }
  std::vector<uint8_t> buf(static_cast<size_t>(fileSize));
  if (std::fread(buf.data(), 1, buf.size(), f) != buf.size()) {
    std::fclose(f);
    if (err) *err = "PSD 读取不完整";
    return false;
  }
  std::fclose(f);

  Cursor c{buf.data(), buf.size(), 0};
  if (c.str(4) != "8BPS") {
    if (err) *err = "不是 PSD 文件（缺 8BPS 签名）";
    return false;
  }
  const uint16_t version = c.u16();
  if (version != 1) {
    if (err) *err = "只支持 PSD（version 1）；该文件是 version " + std::to_string(version) + "（PSB 请用其它工具）";
    return false;
  }
  c.skip(6);
  const uint16_t channels = c.u16();
  const int height = static_cast<int>(c.u32());
  const int width = static_cast<int>(c.u32());
  const int depth = static_cast<int>(c.u16());
  const uint16_t mode = c.u16();
  if (mode != 3) {
    if (err) *err = "只支持 RGB 色彩模式（收到 mode=" + std::to_string(mode) + "）";
    return false;
  }
  if (depth != 8 && depth != 16) {
    if (err) *err = "只支持 8/16 bit 位深（收到 " + std::to_string(depth) + "）";
    return false;
  }
  const int bps = depth == 16 ? 2 : 1;

  PsdInfo info;
  info.width = width;
  info.height = height;
  info.depth = depth;
  info.channels = channels;

  // ---- color mode data ----
  const uint32_t colorModeLen = c.u32();
  c.skip(colorModeLen);
  // ---- image resources ----
  const uint32_t resLen = c.u32();
  c.skip(resLen);
  // ---- layer & mask ----
  const uint32_t sectionLen = c.u32();
  const size_t sectionEnd = c.pos + sectionLen;
  if (sectionEnd > c.n) {
    if (err) *err = "PSD 图层段长度越界";
    return false;
  }
  const uint32_t layerInfoLen = c.u32();
  const size_t layerInfoEnd = c.pos + layerInfoLen;
  const int16_t rawCount = c.i16();
  const bool alphaInFirst = rawCount < 0;  // Photoshop：负数表示首个通道含合并 alpha
  const int layerCount = std::abs(static_cast<int>(rawCount));

  struct PendingChannel {
    int16_t id;
    uint32_t length;
  };
  struct PendingLayer {
    ReadLayer layer;
    std::vector<PendingChannel> channels;
  };
  std::vector<PendingLayer> pending;
  for (int i = 0; i < layerCount; ++i) {
    ReadLayer l;
    l.top = c.i32();
    l.left = c.i32();
    const int bottom = c.i32();
    const int right = c.i32();
    l.height = bottom - l.top;
    l.width = right - l.left;
    if (l.height <= 0 || l.width <= 0) {
      if (err) *err = "图层 #" + std::to_string(i) + " 矩形非法";
      return false;
    }
    const uint16_t chCount = c.u16();
    PendingLayer pl;
    pl.layer = l;
    for (uint16_t k = 0; k < chCount; ++k) {
      PendingChannel pc;
      pc.id = c.i16();
      pc.length = c.u32();
      pl.channels.push_back(pc);
    }
    if (c.str(4) != "8BIM") {
      if (err) *err = "图层 #" + std::to_string(i) + " 缺少 8BIM 签名";
      return false;
    }
    const std::string key = c.str(4);
    pl.layer.blendKey = blendNameFromKey(key);
    pl.layer.opacity = c.u8();
    c.skip(1);  // clipping
    const uint8_t flags = c.u8();
    pl.layer.visible = (flags & 0x02) == 0;
    c.skip(1);  // filler
    const uint32_t extraLen = c.u32();
    const size_t extraEnd = c.pos + extraLen;
    const uint32_t maskLen = c.u32();
    if (maskLen > 0) c.skip(maskLen);
    const uint32_t blendLen = c.u32();
    if (blendLen > 0) c.skip(blendLen);
    // Pascal 名（补齐到 4）
    const uint8_t nameLen = c.u8();
    pl.layer.name = c.str(nameLen);
    const uint32_t nameField = 1u + nameLen;
    c.skip(round4(nameField) - nameField);
    // tagged blocks
    while (c.pos + 12 <= extraEnd) {
      const size_t blockStart = c.pos;
      const std::string sig = c.str(4);
      if (sig != "8BIM" && sig != "8B64") break;
      const std::string bkey = c.str(4);
      const uint32_t dataLen = c.u32();
      if (c.pos + dataLen > c.n) {
        if (err) *err = "tagged block 数据越界";
        return false;
      }
      std::vector<uint8_t> data(c.p + c.pos, c.p + c.pos + dataLen);
      c.skip(dataLen);
      if (bkey == "lumi") {
        pl.layer.luminaSnapshot.assign(data.begin(), data.end());
        info.hasLuminaSnapshot = true;
      } else {
        UnknownBlock ub;
        ub.signature = sig;
        ub.key = bkey;
        ub.data = data;
        pl.layer.unknown.push_back(std::move(ub));
        ++info.unknownBlockCount;
      }
      // padding：块总长 = 4(sig)+4(key)+4(len)+round2(len)（Adobe 规范补齐到偶数）
      const uint32_t consumed = static_cast<uint32_t>(c.pos - blockStart);
      const uint32_t want = 12u + round2(dataLen);
      if (consumed < want) c.skip(want - consumed);
    }
    if (c.pos < extraEnd) c.skip(extraEnd - c.pos);
    pending.push_back(std::move(pl));
  }

  // ---- 通道数据 ----
  for (size_t li = 0; li < pending.size(); ++li) {
    PendingLayer& pl = pending[li];
    std::vector<uint8_t> r, g, b, mask;
    bool hasR = false, hasG = false, hasB = false;
    for (const PendingChannel& pc : pl.channels) {
      const size_t start = c.pos;
      std::vector<uint8_t> plane;
      if (!readChannel(c, pl.layer.width, pl.layer.height, bps, &plane, err)) return false;
      if (pc.id == 0) {
        r = plane;
        hasR = true;
      } else if (pc.id == 1) {
        g = plane;
        hasG = true;
      } else if (pc.id == 2) {
        b = plane;
        hasB = true;
      } else if (pc.id == -2) {
        mask = plane;
      }
      // 通道块实际长度可能与声明的 length 不同（含 padding）；以声明为准对齐
      const size_t consumed = c.pos - start;
      if (consumed < pc.length) c.skip(pc.length - consumed);
    }
    if (!hasR || !hasG || !hasB) {
      if (err) *err = "图层 #" + std::to_string(li) + " 缺少 RGB 通道（无法重建像素）";
      return false;
    }
    interleave(r, g, b, pl.layer.width, pl.layer.height, bps, &pl.layer.rgb);
    pl.layer.mask = std::move(mask);
  }
  (void)alphaInFirst;
  if (c.pos < layerInfoEnd) c.skip(layerInfoEnd - c.pos);
  if (c.pos < sectionEnd) c.skip(sectionEnd - c.pos);
  for (PendingLayer& pl : pending) info.layers.push_back(std::move(pl.layer));

  // ---- 合成图 ----
  if (!c.need(2)) {
    if (err) *err = "缺少合成图数据";
    return false;
  }
  // 注意：Image Data（合成图）与图层通道结构不同 —— 全图只有**一个** compression
  // 字段，其后是「所有通道 × 所有行」的一整张行计数表（不是每通道各带一个头）。
  // 首版按图层通道的口径逐通道读，导致错位（M3-01 调试记录）。
  if (!c.need(2)) {
    if (err) *err = "缺少合成图 compression 字段";
    return false;
  }
  const uint16_t compCompression = c.u16();
  std::vector<std::vector<uint8_t>> compPlanes(3);
  if (compCompression == 0) {
    const size_t planeBytes = static_cast<size_t>(width) * height * bps;
    for (int ch = 0; ch < 3; ++ch) {
      if (!c.need(planeBytes)) {
        if (err) *err = "合成图数据长度不足（Raw）";
        return false;
      }
      compPlanes[ch].assign(c.p + c.pos, c.p + c.pos + planeBytes);
      c.skip(planeBytes);
    }
  } else if (compCompression == 1) {
    const size_t count = static_cast<size_t>(channels) * height;
    if (!c.need(count * 2)) {
      if (err) *err = "合成图行计数表长度不足";
      return false;
    }
    std::vector<uint16_t> rowCounts(count);
    size_t total = 0;
    for (size_t i = 0; i < count; ++i) {
      rowCounts[i] = c.u16();
      total += rowCounts[i];
    }
    if (!c.need(total)) {
      if (err) *err = "合成图数据长度不足（RLE）";
      return false;
    }
    const int rowBytes = width * bps;
    size_t at = c.pos;
    for (int ch = 0; ch < 3; ++ch) {
      std::vector<uint8_t> plane(static_cast<size_t>(width) * height * bps, 0);
      for (int y = 0; y < height; ++y) {
        std::vector<uint8_t> row;
        const uint16_t rc = rowCounts[static_cast<size_t>(ch) * height + y];
        if (!packBitsDecode(c.p + at, rc, &row, rowBytes)) {
          if (err) *err = "合成图 RLE 行解码失败";
          return false;
        }
        if (row.size() != static_cast<size_t>(rowBytes)) {
          if (err) *err = "合成图 RLE 行长度与行宽不符";
          return false;
        }
        std::memcpy(plane.data() + static_cast<size_t>(y) * rowBytes, row.data(), rowBytes);
        at += rc;
      }
      compPlanes[ch] = std::move(plane);
    }
    c.skip(total);
  } else {
    if (err) *err = "不支持的合成图压缩方式: " + std::to_string(compCompression);
    return false;
  }
  interleave(compPlanes[0], compPlanes[1], compPlanes[2], width, height, bps, &info.composite);

  if (out) *out = std::move(info);
  return true;
}

}  // namespace psd
}  // namespace lumina
