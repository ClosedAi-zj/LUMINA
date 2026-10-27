// M3-01：PSD 写出实现（规范口径见 psd_writer.h 文件头）
#include "export/psd_writer.h"

#include <cstdio>
#include <cstring>

#include "core/logger.h"

namespace lumina {
namespace psd {
namespace {

constexpr int kMaxDimension = 30000;  // PSD（version 1）画幅上限；超出必须走 PSB，我们不静默写
constexpr uint32_t kSignature8BIM = 0x3842494D;  // '8BIM'

void putBytes(std::vector<uint8_t>* out, const void* p, size_t n) {
  const uint8_t* s = static_cast<const uint8_t*>(p);
  out->insert(out->end(), s, s + n);
}

void putStr(std::vector<uint8_t>* out, const std::string& s) {
  out->insert(out->end(), s.begin(), s.end());
}

uint32_t round2(uint32_t v) { return (v + 1u) & ~1u; }
uint32_t round4(uint32_t v) { return (v + 3u) & ~3u; }

/** 提取某个通道平面（RGB 交错 → 单通道连续），16bit 时每样本 2 字节原样取 */
std::vector<uint8_t> extractPlane(const std::vector<uint8_t>& interleaved, int w, int h,
                                  int channel, int channels, int bytesPerSample) {
  std::vector<uint8_t> plane(static_cast<size_t>(w) * h * bytesPerSample);
  const size_t stride = static_cast<size_t>(w) * channels * bytesPerSample;
  for (int y = 0; y < h; ++y) {
    const uint8_t* row = interleaved.data() + static_cast<size_t>(y) * stride +
                         static_cast<size_t>(channel) * bytesPerSample;
    for (int x = 0; x < w; ++x) {
      const uint8_t* px = row + static_cast<size_t>(x) * channels * bytesPerSample;
      std::memcpy(plane.data() + (static_cast<size_t>(y) * w + x) * bytesPerSample, px,
                  bytesPerSample);
    }
  }
  return plane;
}

/** 一个通道的编码结果：行计数表 + 数据（RLE 时），或仅数据（Raw 时） */
struct EncodedChannel {
  std::vector<uint16_t> rowCounts;  // RLE 才有；Raw 时为空
  std::vector<uint8_t> data;        // 直接可写盘的字节（Raw = 原平面；RLE = 各行 PackBits 串联）
};

EncodedChannel encodeChannel(const std::vector<uint8_t>& plane, int w, int h, int bytesPerSample,
                             bool rle) {
  EncodedChannel out;
  const int rowBytes = w * bytesPerSample;
  if (!rle) {
    out.data = plane;
    return out;
  }
  out.rowCounts.reserve(static_cast<size_t>(h));
  for (int y = 0; y < h; ++y) {
    std::vector<uint8_t> packed;
    packBitsEncode(plane.data() + static_cast<size_t>(y) * rowBytes, rowBytes, &packed);
    out.rowCounts.push_back(static_cast<uint16_t>(packed.size()));
    out.data.insert(out.data.end(), packed.begin(), packed.end());
  }
  return out;
}

/** 写一行 PackBits 表（RLE 数据之前） */
void putRowCounts(std::vector<uint8_t>* out, const std::vector<uint16_t>& counts) {
  for (uint16_t c : counts) putU16BE(out, c);
}

/** 写 tagged block（含 padding 到偶数；我们自己写的块数据已补齐到 4 的倍数，两种口径等价） */
void putTaggedBlock(std::vector<uint8_t>* out, const std::string& signature, const std::string& key,
                    const std::vector<uint8_t>& data) {
  putStr(out, signature);
  putStr(out, key.size() == 4 ? key : "    ");
  putU32BE(out, static_cast<uint32_t>(data.size()));
  putBytes(out, data.data(), data.size());
  const uint32_t padded = round2(static_cast<uint32_t>(data.size()));
  for (uint32_t i = static_cast<uint32_t>(data.size()); i < padded; ++i) out->push_back(0);
}

/** Pascal 字符串（1 字节长度 + 内容），整体补齐到 4 的倍数 */
void putPascalString4(std::vector<uint8_t>* out, const std::string& s) {
  std::string name = s;
  if (name.size() > 255) name = name.substr(0, 255);
  const uint32_t fieldSize = 1u + static_cast<uint32_t>(name.size());
  out->push_back(static_cast<uint8_t>(name.size()));
  putStr(out, name);
  for (uint32_t i = fieldSize; i < round4(fieldSize); ++i) out->push_back(0);
}

std::vector<uint8_t> buildLayerRecord(const Layer& layer, const WriteOptions& opts,
                                      const std::vector<int16_t>& channelIds,
                                      const std::vector<uint32_t>& channelLengths,
                                      std::string* warning) {
  std::vector<uint8_t> rec;
  putI32BE(&rec, layer.top);
  putI32BE(&rec, layer.left);
  putI32BE(&rec, layer.top + layer.height);
  putI32BE(&rec, layer.left + layer.width);
  putU16BE(&rec, static_cast<uint16_t>(channelIds.size()));
  for (size_t i = 0; i < channelIds.size(); ++i) {
    putI16BE(&rec, channelIds[i]);
    putU32BE(&rec, channelLengths[i]);
  }
  putU32BE(&rec, kSignature8BIM);
  const std::string blend = blendKeyFromName(layer.blendKey, warning);
  putStr(&rec, blend.size() == 4 ? blend : "norm");
  rec.push_back(layer.opacity);
  rec.push_back(0);  // clipping: 0 = base
  uint8_t flags = 0;
  if (!layer.visible) flags |= 0x02;  // Photoshop：bit1 = 隐藏
  rec.push_back(flags);
  rec.push_back(0);  // filler

  // ---- extra data ----
  std::vector<uint8_t> extra;
  const bool hasMask = !layer.mask.empty();
  if (hasMask) {
    putU32BE(&extra, 20);  // layer mask data length
    putI32BE(&extra, layer.top);
    putI32BE(&extra, layer.left);
    putI32BE(&extra, layer.top + layer.height);
    putI32BE(&extra, layer.left + layer.width);
    extra.push_back(255);  // default color: 255 = 白（默认全显示）
    extra.push_back(0);    // flags：0 = 相对图层定位、启用、不反相
    extra.push_back(0);
    extra.push_back(0);    // padding → 16+1+1+2 = 20
  } else {
    putU32BE(&extra, 0);
  }
  // blending ranges：4 通道 × 8 字节 + 用户蒙版 8 字节 = 40（全 0/255 默认区间）
  putU32BE(&extra, 40);
  for (int i = 0; i < 5; ++i) {
    extra.push_back(0);
    extra.push_back(255);
    extra.push_back(0);
    extra.push_back(255);
    extra.push_back(0);
    extra.push_back(255);
    extra.push_back(0);
    extra.push_back(255);
  }
  putPascalString4(&extra, layer.name);
  if (!layer.luminaSnapshot.empty()) {
    // 数据补齐到 4 的倍数：让 round2 与常见的 round4 解析等价（见文件头 padding 说明）
    std::vector<uint8_t> data(layer.luminaSnapshot.begin(), layer.luminaSnapshot.end());
    const uint32_t want = round4(static_cast<uint32_t>(data.size()));
    while (data.size() < want) data.push_back(' ');
    putTaggedBlock(&extra, "8BIM", "lumi", data);
  }
  for (const UnknownBlock& ub : layer.unknown) {
    putTaggedBlock(&extra, ub.signature, ub.key, ub.data);
  }
  putU32BE(&rec, static_cast<uint32_t>(extra.size()));
  rec.insert(rec.end(), extra.begin(), extra.end());
  return rec;
}

}  // namespace

void putU16BE(std::vector<uint8_t>* out, uint16_t v) {
  out->push_back(static_cast<uint8_t>(v >> 8));
  out->push_back(static_cast<uint8_t>(v & 0xFF));
}
void putU32BE(std::vector<uint8_t>* out, uint32_t v) {
  out->push_back(static_cast<uint8_t>(v >> 24));
  out->push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
  out->push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
  out->push_back(static_cast<uint8_t>(v & 0xFF));
}
void putI16BE(std::vector<uint8_t>* out, int16_t v) {
  putU16BE(out, static_cast<uint16_t>(v));
}
void putI32BE(std::vector<uint8_t>* out, int32_t v) {
  putU32BE(out, static_cast<uint32_t>(v));
}

std::string blendKeyFromName(const std::string& name, std::string* warning) {
  struct Map {
    const char* name;
    const char* key;
  };
  static const Map kMap[] = {
      {"normal", "norm"}, {"multiply", "mul "},  {"screen", "scrn"},  {"overlay", "over"},
      {"softLight", "sLit"}, {"hardLight", "hLit"}, {"lighten", "lite"}, {"darken", "dark"},
      {"luminosity", "lum "}, {"color", "hue "},  {"norm", "norm"},   {"mul ", "mul "},
      {"scrn", "scrn"},    {"over", "over"},     {"sLit", "sLit"},   {"hLit", "hLit"},
      {"lite", "lite"},    {"dark", "dark"},     {"lum ", "lum "},   {"hue ", "hue "},
  };
  for (const Map& m : kMap) {
    if (name == m.name) return m.key;
  }
  if (warning && !name.empty()) {
    *warning += "混合模式未收录，已回退 normal: " + name + "；";
  }
  return "norm";
}

void packBitsEncode(const uint8_t* src, int len, std::vector<uint8_t>* out) {
  // PackBits：字面段写 (n-1) + n 字节；重复段写 (257 - run) + 1 字节（run 3..128）
  int i = 0;
  std::vector<uint8_t> literal;
  auto flushLiteral = [&]() {
    size_t pos = 0;
    while (pos < literal.size()) {
      const size_t n = std::min<size_t>(128, literal.size() - pos);
      out->push_back(static_cast<uint8_t>(n - 1));
      out->insert(out->end(), literal.begin() + pos, literal.begin() + pos + n);
      pos += n;
    }
    literal.clear();
  };
  while (i < len) {
    if (i + 2 < len && src[i] == src[i + 1] && src[i] == src[i + 2]) {
      flushLiteral();
      int run = 3;
      while (run < 128 && i + run < len && src[i + run] == src[i]) ++run;
      out->push_back(static_cast<uint8_t>(257 - run));
      out->push_back(src[i]);
      i += run;
    } else {
      literal.push_back(src[i]);
      ++i;
    }
  }
  flushLiteral();
}

bool packBitsDecode(const uint8_t* src, int len, std::vector<uint8_t>* out, int expected) {
  int i = 0;
  while (i < len) {
    const int8_t header = static_cast<int8_t>(src[i++]);
    if (header >= 0) {
      const int n = header + 1;
      if (i + n > len) return false;
      out->insert(out->end(), src + i, src + i + n);
      i += n;
    } else {
      const int n = 1 - header;  // header 为 -1..-128 → n = 2..129
      if (i + 1 > len) return false;
      const uint8_t v = src[i++];
      out->insert(out->end(), n, v);
    }
  }
  return expected < 0 || static_cast<int>(out->size()) == expected;
}

bool writePsd(const std::string& path, const Document& doc, const WriteOptions& opts,
              std::string* err) {
  const int bps = opts.depth == 16 ? 2 : 1;
  if (opts.depth != 8 && opts.depth != 16) {
    if (err) *err = "PSD 位深只支持 8 或 16（收到 " + std::to_string(opts.depth) + "）";
    return false;
  }
  if (opts.width <= 0 || opts.height <= 0 || opts.width > kMaxDimension ||
      opts.height > kMaxDimension) {
    if (err) *err = "PSD 画幅非法或超出 version 1 上限（30000）：" + std::to_string(opts.width) + "x" + std::to_string(opts.height);
    return false;
  }
  if (doc.layers.empty()) {
    if (err) *err = "PSD 至少需要 1 个图层";
    return false;
  }
  const size_t expectComposite = static_cast<size_t>(opts.width) * opts.height * 3 * bps;
  if (opts.writeComposite && doc.composite.size() != expectComposite) {
    if (err) *err = "合并图像素长度与画幅不符：期望 " + std::to_string(expectComposite) + " 实到 " + std::to_string(doc.composite.size());
    return false;
  }
  std::string warning;
  for (size_t li = 0; li < doc.layers.size(); ++li) {
    const Layer& l = doc.layers[li];
    const size_t expect = static_cast<size_t>(l.width) * l.height * 3 * bps;
    if (l.width <= 0 || l.height <= 0) {
      if (err) *err = "图层 #" + std::to_string(li) + " 尺寸非法";
      return false;
    }
    if (l.rgb.size() != expect) {
      if (err) *err = "图层 #" + std::to_string(li) + " 像素长度与尺寸不符：期望 " + std::to_string(expect) + " 实到 " + std::to_string(l.rgb.size());
      return false;
    }
    if (!l.mask.empty() && l.mask.size() != static_cast<size_t>(l.width) * l.height * bps) {
      if (err) *err = "图层 #" + std::to_string(li) + " 蒙版长度与尺寸不符";
      return false;
    }
    if (l.left < 0 || l.top < 0 || l.left + l.width > opts.width || l.top + l.height > opts.height) {
      if (err) *err = "图层 #" + std::to_string(li) + " 矩形超出画布（PSD 图层不得越界）";
      return false;
    }
  }

  std::FILE* f = std::fopen(path.c_str(), "wb");
  if (!f) {
    if (err) *err = "无法写入 PSD 文件: " + path;
    return false;
  }
  auto fail = [&](const std::string& msg) {
    std::fclose(f);
    if (err) *err = msg;
    return false;
  };
  auto write = [&](const std::vector<uint8_t>& buf) {
    return buf.empty() || std::fwrite(buf.data(), 1, buf.size(), f) == buf.size();
  };

  // ---- 1. File header（26 字节） ----
  std::vector<uint8_t> head;
  putStr(&head, "8BPS");
  putU16BE(&head, 1);  // version
  for (int i = 0; i < 6; ++i) head.push_back(0);
  putU16BE(&head, 3);  // channels = RGB
  putU32BE(&head, static_cast<uint32_t>(opts.height));
  putU32BE(&head, static_cast<uint32_t>(opts.width));
  putU16BE(&head, static_cast<uint16_t>(opts.depth));
  putU16BE(&head, 3);  // color mode = RGB
  if (!write(head)) return fail("PSD 文件头写入失败");

  // ---- 2. Color mode data（空） ----
  std::vector<uint8_t> four;
  putU32BE(&four, 0);
  if (!write(four)) return fail("PSD color mode 写入失败");

  // ---- 3. Image resources（可选 ResolutionInfo 0x03ED，提升 PS 兼容） ----
  std::vector<uint8_t> resBlock;
  if (opts.dpi > 0) {
    const uint32_t fixed = static_cast<uint32_t>(opts.dpi) << 16;
    std::vector<uint8_t> data;
    putU32BE(&data, fixed);
    putU16BE(&data, 1);  // hResUnit = dpi
    putU16BE(&data, 1);  // widthUnit = inch
    putU32BE(&data, fixed);
    putU16BE(&data, 1);  // vResUnit
    putU16BE(&data, 1);  // heightUnit
    putStr(&resBlock, "8BIM");
    putU16BE(&resBlock, 0x03ED);
    putU16BE(&resBlock, 0);  // 空名（Pascal，补齐到偶）
    putU32BE(&resBlock, static_cast<uint32_t>(data.size()));
    putBytes(&resBlock, data.data(), data.size());
  }
  std::vector<uint8_t> resLen;
  putU32BE(&resLen, static_cast<uint32_t>(resBlock.size()));
  if (!write(resLen) || !write(resBlock)) return fail("PSD image resources 写入失败");

  // ---- 4. Layer and mask information（长度先占位，写完回填） ----
  const long sectionLenPos = std::ftell(f);
  if (sectionLenPos < 0) return fail("PSD 定位失败（ftell）");
  if (!write(four)) return fail("PSD layer section 长度占位写入失败");
  const long sectionStart = std::ftell(f);

  // -- 4a. layer info（长度同样回填） --
  const long layerInfoLenPos = std::ftell(f);
  if (!write(four)) return fail("PSD layer info 长度占位写入失败");
  const long layerInfoStart = std::ftell(f);

  std::vector<uint8_t> count;
  putI16BE(&count, static_cast<int16_t>(doc.layers.size()));
  if (!write(count)) return fail("PSD 图层数写入失败");

  // 先写全部图层记录（记录里带各通道数据长度），再依次写通道数据
  std::vector<std::vector<EncodedChannel>> channelsPerLayer;
  channelsPerLayer.reserve(doc.layers.size());
  std::vector<std::vector<int16_t>> idsPerLayer;
  for (const Layer& l : doc.layers) {
    const bool hasMask = !l.mask.empty();
    std::vector<int16_t> ids = {0, 1, 2};
    if (hasMask) ids.push_back(-2);
    std::vector<EncodedChannel> enc;
    for (int c = 0; c < 3; ++c) {
      const std::vector<uint8_t> plane = extractPlane(l.rgb, l.width, l.height, c, 3, bps);
      enc.push_back(encodeChannel(plane, l.width, l.height, bps, opts.rle));
    }
    if (hasMask) {
      enc.push_back(encodeChannel(l.mask, l.width, l.height, bps, opts.rle));
    }
    // 通道数据长度 = [compression u16] + [行计数表] + [数据]
    std::vector<uint32_t> lengths;
    for (const EncodedChannel& e : enc) {
      const uint32_t rowTable = opts.rle ? static_cast<uint32_t>(e.rowCounts.size()) * 2u : 0u;
      lengths.push_back(2u + rowTable + static_cast<uint32_t>(e.data.size()));
    }
    const std::vector<uint8_t> rec = buildLayerRecord(l, opts, ids, lengths, &warning);
    if (!write(rec)) return fail("PSD 图层记录写入失败");
    channelsPerLayer.push_back(std::move(enc));
    idsPerLayer.push_back(std::move(ids));
  }

  for (size_t li = 0; li < doc.layers.size(); ++li) {
    const Layer& l = doc.layers[li];
    for (const EncodedChannel& e : channelsPerLayer[li]) {
      std::vector<uint8_t> chan;
      putU16BE(&chan, opts.rle ? 1 : 0);
      if (opts.rle) putRowCounts(&chan, e.rowCounts);
      if (!write(chan)) return fail("PSD 通道头写入失败");
      if (!e.data.empty() && std::fwrite(e.data.data(), 1, e.data.size(), f) != e.data.size()) {
        return fail("PSD 通道数据写入失败");
      }
    }
    (void)l;
  }

  const long layerInfoEnd = std::ftell(f);
  // layer info 长度需偶对齐（Photoshop 要求）
  const long layerInfoLen = layerInfoEnd - layerInfoStart;
  if (layerInfoLen % 2 != 0) {
    const uint8_t pad = 0;
    if (std::fwrite(&pad, 1, 1, f) != 1) return fail("PSD 对齐写入失败");
  }
  if (std::fseek(f, layerInfoLenPos, SEEK_SET) != 0) return fail("PSD 回填定位失败");
  std::vector<uint8_t> back;
  putU32BE(&back, static_cast<uint32_t>(layerInfoLen + (layerInfoLen % 2)));
  if (!write(back)) return fail("PSD layer info 长度回填失败");
  if (std::fseek(f, layerInfoEnd + (layerInfoLen % 2), SEEK_SET) != 0) return fail("PSD 定位失败");

  // -- 4b. global mask info（空） --
  if (!write(four)) return fail("PSD global mask 写入失败");

  const long sectionEnd = std::ftell(f);
  const long sectionLen = sectionEnd - sectionStart;
  if (sectionLen % 2 != 0) {
    const uint8_t pad = 0;
    if (std::fwrite(&pad, 1, 1, f) != 1) return fail("PSD 对齐写入失败");
  }
  if (std::fseek(f, sectionLenPos, SEEK_SET) != 0) return fail("PSD 回填定位失败");
  std::vector<uint8_t> back2;
  putU32BE(&back2, static_cast<uint32_t>(sectionLen + (sectionLen % 2)));
  if (!write(back2)) return fail("PSD layer section 长度回填失败");
  if (std::fseek(f, sectionEnd + (sectionLen % 2), SEEK_SET) != 0) return fail("PSD 定位失败");

  // ---- 5. Image data（合并图） ----
  if (opts.writeComposite) {
    std::vector<uint8_t> compHead;
    putU16BE(&compHead, opts.rle ? 1 : 0);
    std::vector<EncodedChannel> comp;
    std::vector<std::vector<uint8_t>> planes;
    for (int c = 0; c < 3; ++c) {
      planes.push_back(extractPlane(doc.composite, opts.width, opts.height, c, 3, bps));
      comp.push_back(encodeChannel(planes.back(), opts.width, opts.height, bps, opts.rle));
      if (opts.rle) putRowCounts(&compHead, comp.back().rowCounts);
    }
    if (!write(compHead)) return fail("PSD 合成图头写入失败");
    for (const EncodedChannel& e : comp) {
      if (!e.data.empty() && std::fwrite(e.data.data(), 1, e.data.size(), f) != e.data.size()) {
        return fail("PSD 合成图数据写入失败");
      }
    }
  } else {
    std::vector<uint8_t> compHead;
    putU16BE(&compHead, 0);  // Raw
    std::vector<uint8_t> zeros(static_cast<size_t>(opts.width) * opts.height * 3 * bps, 0);
    if (!write(compHead) || (std::fwrite(zeros.data(), 1, zeros.size(), f) != zeros.size())) {
      return fail("PSD 合成图（空）写入失败");
    }
  }

  std::fclose(f);
  if (!warning.empty()) {
    logWarn("psd_writer", "PSD 写出告警: " + warning);
  }
  return true;
}

}  // namespace psd
}  // namespace lumina
