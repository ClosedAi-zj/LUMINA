// 模拟图库生成器实现
// 注意：Panasonic RW2 为私有压缩容器无法程序化构造；本模块生成"结构合法的未压缩 Linear DNG"
//       （TIFF-EP：IFD + SubIFD 预览，Photometric=32803 CFA，RGGB，16bit，Compression=1），
//       使 LibRaw 能真解码（L1 验证不伪造）。接真实图库时 LibRaw 直解 RW2，代码无需改动。
#include "mock/generator.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#include "core/jpeg_encoder.h"  // 与导出队列共用的 JPEG 编码层（字节一致）
#include "core/logger.h"
#include "core/md5.h"
#include "core/str_util.h"

namespace lumina {
namespace mock {

MockGenerator::MockGenerator(core::Config* config, db::PhotoDao* photos, db::Database* db,
                             db::FtsIndexer* fts, db::HealthDao* health, raw::RawDecoder* decoder,
                             xmp::XmpService* xmp)
    : config_(config),
      photos_(photos),
      db_(db),
      fts_(fts),
      health_(health),
      decoder_(decoder),
      xmp_(xmp) {}

namespace {

// ---- 确定性伪随机（同一种子必得同一张图，便于回归） ----
struct Lcg {
  uint32_t state;
  explicit Lcg(uint32_t seed) : state(seed ? seed : 0x9E3779B9u) {}
  uint32_t next() {
    state = state * 1664525u + 1013904223u;
    return state;
  }
  double unit() { return static_cast<double>(next() % 100000) / 100000.0; }
  double range(double lo, double hi) { return lo + unit() * (hi - lo); }
};

// 程序化合成 RGB：风光 / 建筑 / 夜景 / 山岚 / 几何 五种题材风格
void synthesizeRgb(std::vector<uint8_t>* rgb, int w, int h, int style, uint32_t seed) {
  rgb->assign(static_cast<size_t>(w) * h * 3, 0);
  Lcg rng(seed);
  const double horizon = h * (style == 1 ? 0.62 : 0.55);
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      size_t i = (static_cast<size_t>(y) * w + x) * 3;
      double fx = static_cast<double>(x) / w;
      double fy = static_cast<double>(y) / h;
      int r = 0, g = 0, b = 0;
      if (style == 0) {
        double t = fy / 0.55;
        r = static_cast<int>(40 + 150 * (1.0 - t) * t + 60 * (1.0 - fy));
        g = static_cast<int>(90 + 110 * (1.0 - t));
        b = static_cast<int>(140 + 80 * t);
        double ridge = horizon + std::sin(fx * 6.2831 * 2.0 + seed * 0.001) * h * 0.10 +
                       std::sin(fx * 6.2831 * 5.0) * h * 0.03;
        if (y > ridge) {
          double depth = (y - ridge) / (h - ridge + 1);
          int base = static_cast<int>(60 - 45 * depth);
          r = static_cast<int>(base * 0.72);
          g = static_cast<int>(base * 0.78);
          b = static_cast<int>(base * 0.95);
          double fog = std::exp(-std::pow((y - ridge - h * 0.06) / (h * 0.05), 2.0));
          r = static_cast<int>(r + (200 - r) * fog * 0.5);
          g = static_cast<int>(g + (205 - g) * fog * 0.5);
          b = static_cast<int>(b + (215 - b) * fog * 0.5);
        }
      } else if (style == 1) {
        int col = x / 12;
        int row = y / 14;
        bool window = (x % 12 > 3 && x % 12 < 10) && (y % 14 > 4 && y % 14 < 11);
        double shade = 0.35 + 0.4 * (1.0 - fx);
        r = static_cast<int>(190 * shade);
        g = static_cast<int>(185 * shade);
        b = static_cast<int>(170 * shade);
        if (window) {
          double glow = ((col * 7 + row * 13) % 5) / 4.0;
          r = static_cast<int>(120 + 120 * glow);
          g = static_cast<int>(140 + 100 * glow);
          b = static_cast<int>(90 + 140 * glow);
        }
      } else if (style == 2) {
        double shade = 0.12 + 0.25 * (1.0 - fy);
        r = static_cast<int>(60 * shade);
        g = static_cast<int>(70 * shade);
        b = static_cast<int>(110 * shade);
        if ((x * 31 + y * 17) % 211 == 0) {
          r = 250;
          g = 220;
          b = 140;
        }
      } else if (style == 3) {
        double t = fy;
        double bands = 0.5 + 0.5 * std::sin(t * 18.0 + fx * 4.0);
        r = static_cast<int>(120 + 70 * bands * (1.0 - t));
        g = static_cast<int>(130 + 60 * bands);
        b = static_cast<int>(150 + 50 * (1.0 - t));
      } else {
        double stripe = std::fmod(fx * 8.0 + fy * 3.0, 1.0);
        int v = static_cast<int>(stripe > 0.5 ? 215 : 45);
        r = v;
        g = static_cast<int>(v * 0.92);
        b = static_cast<int>(v * 0.85);
      }
      int noise = static_cast<int>(rng.range(-4.0, 4.0));
      auto clampAdd = [](int v) -> int {
        if (v < 0) return 0;
        if (v > 255) return 255;
        return v;
      };
      (*rgb)[i + 0] = static_cast<uint8_t>(clampAdd(r + noise));
      (*rgb)[i + 1] = static_cast<uint8_t>(clampAdd(g + noise));
      (*rgb)[i + 2] = static_cast<uint8_t>(clampAdd(b + noise));
    }
  }
}

// RGGB 采样降为单通道 16bit CFA
std::vector<uint16_t> rgbToCfa(const std::vector<uint8_t>& rgb, int w, int h) {
  std::vector<uint16_t> cfa(static_cast<size_t>(w) * h, 0);
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      size_t i = (static_cast<size_t>(y) * w + x) * 3;
      int r = rgb[i + 0];
      int g = rgb[i + 1];
      int b = rgb[i + 2];
      uint16_t v = 0;
      if ((y & 1) == 0 && (x & 1) == 0) v = static_cast<uint16_t>(r << 8);
      else if ((y & 1) == 1 && (x & 1) == 1) v = static_cast<uint16_t>(b << 8);
      else v = static_cast<uint16_t>(g << 8);
      cfa[static_cast<size_t>(y) * w + x] = v;
    }
  }
  return cfa;
}

void put16(std::string* b, uint16_t v) {
  b->push_back(static_cast<char>(v & 0xFF));
  b->push_back(static_cast<char>((v >> 8) & 0xFF));
}
void put32(std::string* b, uint32_t v) {
  b->push_back(static_cast<char>(v & 0xFF));
  b->push_back(static_cast<char>((v >> 8) & 0xFF));
  b->push_back(static_cast<char>((v >> 16) & 0xFF));
  b->push_back(static_cast<char>((v >> 24) & 0xFF));
}
void putAscii(std::string* b, const std::string& s) {
  b->append(s);
  b->push_back('\0');
}
size_t align2(size_t v) { return (v + 1) & ~static_cast<size_t>(1); }

// "YYYY-MM-DD HH:MM:SS" → epoch ms（UTC）
int64_t parseMsFromDate(const std::string& text) {
  int y = 0, mo = 0, d = 0, h = 0, mi = 0, s = 0;
  if (sscanf(text.c_str(), "%d-%d-%d %d:%d:%d", &y, &mo, &d, &h, &mi, &s) < 3) return 0;
  if (y < 1970 || mo < 1 || mo > 12 || d < 1 || d > 31) return 0;
  static const int kDaysBeforeMonth[13] = {0,   0,   31,  59,  90,  120, 151,
                                           181, 212, 243, 273, 304, 334};
  int64_t days = 365 * (y - 1970) + (y - 1969) / 4;
  days += kDaysBeforeMonth[mo] + (d - 1);
  if (mo > 2 && ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0)) ++days;
  int64_t secs = days * 86400 + h * 3600 + mi * 60 + s;
  return secs * 1000;
}

// TIFF IFD 条目
struct Entry {
  uint16_t tag;
  uint16_t type;  // 1=BYTE 2=ASCII 3=SHORT 4=LONG 5=RATIONAL
  uint32_t count;
  uint32_t inlineValue;  // ≤4 字节时直接写；否则为值区偏移（调用前算好）
};

void writeIfd(std::string* f, const std::vector<Entry>& entries) {
  put16(f, static_cast<uint16_t>(entries.size()));
  for (const Entry& e : entries) {
    put16(f, e.tag);
    put16(f, e.type);
    put32(f, e.count);
    put32(f, e.inlineValue);
  }
  put32(f, 0);  // next IFD
}

}  // namespace

bool MockSpec::fromJson(const json::JsonValue& params, MockSpec* out, std::string* err) {
  MockSpec s;
  if (params.isObject()) {
    const json::JsonValue& spec = params.get("spec");
    if (spec.isObject()) {
      if (spec.contains("total")) s.total = spec.get("total").asInt(s.total);
      if (spec.contains("rootCount")) s.rootCount = static_cast<int>(spec.get("rootCount").asInt(s.rootCount));
      if (spec.contains("folderCount")) s.folderCount = static_cast<int>(spec.get("folderCount").asInt(s.folderCount));
      if (spec.contains("missing")) s.missing = spec.get("missing").asInt(s.missing);
      if (spec.contains("edited")) s.edited = spec.get("edited").asInt(s.edited);
      if (spec.contains("gps")) s.gps = spec.get("gps").asInt(s.gps);
      if (spec.contains("maskRatio")) s.maskRatio = spec.get("maskRatio").asDouble(s.maskRatio);
      if (spec.contains("rw2Ratio")) s.rw2Ratio = spec.get("rw2Ratio").asDouble(s.rw2Ratio);
      if (spec.contains("imageWidth")) s.imageWidth = static_cast<int>(spec.get("imageWidth").asInt(s.imageWidth));
      if (spec.contains("imageHeight")) s.imageHeight = static_cast<int>(spec.get("imageHeight").asInt(s.imageHeight));
      if (spec.contains("embedPreview")) s.embedPreview = spec.get("embedPreview").asBool(s.embedPreview);
      if (spec.contains("pairCount")) s.pairCount = static_cast<int>(spec.get("pairCount").asInt(s.pairCount));
      if (spec.contains("bracketGroups")) {
        s.bracketGroups = static_cast<int>(spec.get("bracketGroups").asInt(s.bracketGroups));
      }
      if (spec.contains("libraryRoot")) s.libraryRoot = spec.get("libraryRoot").asString(s.libraryRoot);
      const json::JsonValue& monthRange = spec.get("monthRange");
      if (monthRange.isArray() && monthRange.size() >= 2) {
        s.monthFrom = monthRange.at(0).asString(s.monthFrom);
        s.monthTo = monthRange.at(1).asString(s.monthTo);
      }
    }
  }
  if (s.total <= 0) {
    if (err) *err = "spec.total must be > 0";
    return false;
  }
  if (s.imageWidth < 16 || s.imageHeight < 16) {
    if (err) *err = "imageWidth/imageHeight too small (min 16)";
    return false;
  }
  if (s.folderCount < 1) s.folderCount = 1;
  if (s.rootCount < 1) s.rootCount = 1;
  if (s.bracketGroups < 0) s.bracketGroups = 0;  // 上限在 run() 里按 total/3 截断
  *out = s;
  return true;
}

bool MockGenerator::writeJpegAsset(const std::string& path, int width, int height, int style,
                                   uint32_t seed) {
  std::vector<uint8_t> rgb;
  synthesizeRgb(&rgb, width, height, style, seed);
  imaging::Image image;
  image.width = width;
  image.height = height;
  image.pixels = rgb;
  std::string err;
  return core::jpeg::encodeToFile(path, image, 88, &err);
}

bool MockGenerator::writeLinearDng(const std::string& path, int width, int height, int style,
                                   uint32_t seed, const std::string& dateTime, int iso,
                                   double aperture, double shutter, int focal) {
  std::vector<uint8_t> rgb;
  synthesizeRgb(&rgb, width, height, style, seed);
  std::vector<uint16_t> cfa = rgbToCfa(rgb, width, height);
  const size_t cfaBytes = cfa.size() * 2;

  // 可选：内嵌 JPEG 预览（DNG SubIFD，NewSubFileType=1）→ raw.getProxy 可用
  std::string previewData;  // 预览 JPEG 字节（与导出同一编码层，质量 85）
  int previewW = 80, previewH = 56;
  if (embedPreview_) {
    imaging::Image src;
    src.width = width;
    src.height = height;
    src.pixels = rgb;
    imaging::Image scaled;
    imaging::resizeImage(src, &scaled, previewW, previewH);
    std::string jpegErr;
    if (!core::jpeg::encodeToMemory(scaled, 85, &previewData, &jpegErr)) {
      logWarn("mock", "preview jpeg encode failed for " + path + ": " + jpegErr);
      previewData.clear();
    }
  }

  // ---- 布局预计算（先算全部偏移，再顺序写） ----
  const std::string desc = "LUMINA mock asset - Linear DNG (uncompressed CFA)";
  const std::string make = "Panasonic";
  const std::string model = "DC-S5M2";
  const std::string software = "LUMINA 1.0.0";
  const std::string uniqueModel = "Panasonic DC-S5M2";

  std::vector<Entry> ifd0;
  auto shortVal = [&](uint16_t tag, uint16_t v) { ifd0.push_back(Entry{tag, 3, 1, v}); };
  auto longVal = [&](uint16_t tag, uint32_t v) { ifd0.push_back(Entry{tag, 4, 1, v}); };
  auto asciiVal = [&](uint16_t tag, const std::string& s) {
    ifd0.push_back(Entry{tag, 2, static_cast<uint32_t>(s.size() + 1), 0});
  };

  shortVal(0x0100, static_cast<uint16_t>(width));
  shortVal(0x0101, static_cast<uint16_t>(height));
  shortVal(0x0102, 16);    // BitsPerSample
  shortVal(0x0103, 1);     // Compression = 未压缩
  shortVal(0x0106, 32803); // PhotometricInterpretation = CFA
  asciiVal(0x010e, desc);
  asciiVal(0x010f, make);
  asciiVal(0x0110, model);
  longVal(0x0111, 0);  // StripOffsets（回填）
  shortVal(0x0112, 1); // Orientation
  shortVal(0x0115, 1); // SamplesPerPixel
  longVal(0x0117, static_cast<uint32_t>(cfaBytes));  // StripByteCounts
  asciiVal(0x0131, software);
  asciiVal(0x0132, dateTime);
  asciiVal(0xc614, uniqueModel);
  ifd0.push_back(Entry{0xc612, 1, 4, 0x00040001u});  // DNGVersion 1.4.0.0
  shortVal(0xc61a, 0);      // BlackLevel
  shortVal(0xc61d, 65535);  // WhiteLevel
  shortVal(0x8827, static_cast<uint16_t>(iso));  // ISO
  // RATIONAL：ExposureTime / FNumber / FocalLength（放值区）
  ifd0.push_back(Entry{0x829a, 5, 1, 0});
  ifd0.push_back(Entry{0x829d, 5, 1, 0});
  ifd0.push_back(Entry{0x920a, 5, 1, 0});
  // CFA 描述
  ifd0.push_back(Entry{0x828d, 3, 2, 0});  // CFARepeatPatternDim = {2,2}（值区）
  ifd0.push_back(Entry{0x828e, 1, 4, 0x02010001u});  // CFAPattern = 0,1,1,2（RGGB，内联）
  if (!previewData.empty()) {
    longVal(0x014a, 0);  // SubIFDs（回填）
  }
  std::sort(ifd0.begin(), ifd0.end(),
            [](const Entry& a, const Entry& b) { return a.tag < b.tag; });

  const size_t ifd0Offset = 8;
  const size_t ifd0Size = 2 + ifd0.size() * 12 + 4;
  size_t valuesOffset = ifd0Offset + ifd0Size;  // 必为偶数

  // 值区：字符串 → SHORT[2] → 3×RATIONAL
  size_t stringsOffset = valuesOffset;
  size_t stringsBytes = 0;
  for (const std::string& s : {desc, make, model, software, dateTime, uniqueModel}) {
    stringsBytes += align2(s.size() + 1);
  }
  size_t short2Offset = stringsOffset + stringsBytes;
  size_t rationalOffset = short2Offset + 4;

  size_t cfaOffset = align2(rationalOffset + 24);
  size_t ifd1Offset = 0;
  size_t ifd1Size = 0;
  size_t bitsOffset = 0;
  size_t jpegOffset = 0;
  std::vector<Entry> ifd1;
  if (!previewData.empty()) {
    ifd1.push_back(Entry{0x00fe, 4, 1, 1});                            // NewSubFileType = 预览
    ifd1.push_back(Entry{0x0100, 3, 1, static_cast<uint32_t>(previewW)});
    ifd1.push_back(Entry{0x0101, 3, 1, static_cast<uint32_t>(previewH)});
    ifd1.push_back(Entry{0x0102, 3, 3, 0});                            // BitsPerSample 8,8,8
    ifd1.push_back(Entry{0x0103, 3, 1, 6});                            // JPEG
    ifd1.push_back(Entry{0x0106, 3, 1, 6});                            // YCbCr
    ifd1.push_back(Entry{0x0111, 4, 1, 0});                            // StripOffsets
    ifd1.push_back(Entry{0x0115, 3, 1, 3});                            // SamplesPerPixel
    ifd1.push_back(Entry{0x0117, 4, 1, static_cast<uint32_t>(previewData.size())});
    ifd1.push_back(Entry{0x0201, 4, 1, 0});                            // JPEGInterchangeFormat
    ifd1.push_back(Entry{0x0202, 4, 1, static_cast<uint32_t>(previewData.size())});
    std::sort(ifd1.begin(), ifd1.end(),
              [](const Entry& a, const Entry& b) { return a.tag < b.tag; });
    ifd1Offset = align2(cfaOffset + cfaBytes);
    ifd1Size = 2 + ifd1.size() * 12 + 4;
    bitsOffset = ifd1Offset + ifd1Size;
    jpegOffset = align2(bitsOffset + 6);
  }

  // 回填需要偏移的条目：字符串按 IFD0 中 ASCII 条目出现顺序依次分配
  std::vector<std::string> stringOrder = {desc, make, model, software, dateTime, uniqueModel};
  size_t strCursor = stringsOffset;
  for (Entry& e : ifd0) {
    if (e.type != 2) continue;
    if (stringOrder.empty()) break;
    e.inlineValue = static_cast<uint32_t>(strCursor);
    strCursor += align2(stringOrder.front().size() + 1);
    stringOrder.erase(stringOrder.begin());
  }
  for (Entry& e : ifd0) {
    if (e.tag == 0x828d) e.inlineValue = static_cast<uint32_t>(short2Offset);
    if (e.tag == 0x0111) e.inlineValue = static_cast<uint32_t>(cfaOffset);
    if (e.tag == 0x014a) e.inlineValue = static_cast<uint32_t>(ifd1Offset);
    if (e.tag == 0x829a) e.inlineValue = static_cast<uint32_t>(rationalOffset);
    if (e.tag == 0x829d) e.inlineValue = static_cast<uint32_t>(rationalOffset + 8);
    if (e.tag == 0x920a) e.inlineValue = static_cast<uint32_t>(rationalOffset + 16);
  }
  for (Entry& e : ifd1) {
    if (e.tag == 0x0102) e.inlineValue = static_cast<uint32_t>(bitsOffset);
    if (e.tag == 0x0111 || e.tag == 0x0201) e.inlineValue = static_cast<uint32_t>(jpegOffset);
  }

  // ---- 顺序写文件 ----
  std::string f;
  f.reserve(4096 + cfaBytes + previewData.size());
  put16(&f, 0x4949);  // II
  put16(&f, 42);
  put32(&f, static_cast<uint32_t>(ifd0Offset));
  writeIfd(&f, ifd0);
  // 字符串值区
  for (const std::string& s : {desc, make, model, software, dateTime, uniqueModel}) {
    putAscii(&f, s);
    if (f.size() % 2 != 0) f.push_back('\0');
  }
  // CFARepeatPatternDim
  put16(&f, 2);
  put16(&f, 2);
  // RATIONAL 值区
  uint32_t shutterDen = shutter > 0.0 ? static_cast<uint32_t>(1.0 / shutter + 0.5) : 1;
  if (shutterDen == 0) shutterDen = 1;
  put32(&f, 1);
  put32(&f, shutterDen);
  put32(&f, static_cast<uint32_t>(aperture * 10.0 + 0.5));
  put32(&f, 10);
  put32(&f, static_cast<uint32_t>(focal));
  put32(&f, 1);
  // CFA 数据
  f.append(reinterpret_cast<const char*>(cfa.data()), cfaBytes);
  if (!previewData.empty()) {
    if (f.size() % 2 != 0) f.push_back('\0');
    writeIfd(&f, ifd1);
    put16(&f, 8);
    put16(&f, 8);
    put16(&f, 8);
    if (f.size() % 2 != 0) f.push_back('\0');
    f.append(previewData);
  }

  std::string err;
  if (!writeFileAtomic(path, f, &err)) {
    logError("mock", "write dng failed: " + err);
    return false;
  }
  return true;
}

void MockGenerator::writeMockReadme(const std::string& libraryRoot) {
  static const char* kReadme =
      "# LUMINA 模拟图库（Mock Library）\n"
      "\n"
      "本目录由 `mock.generate` 程序化生成，用于开发 / 测试 / 演示。\n"
      "\n"
      "## 关于影像本体\n"
      "\n"
      "- Panasonic RW2 是私有压缩容器，**无法程序化构造**；因此本图库的 RAW 影像本体为"
      "**结构合法的未压缩 Linear DNG**（TIFF-EP：单 IFD + SubIFD 预览，"
      "PhotometricInterpretation=32803 CFA，CFAPattern RGGB，BitsPerSample=16，"
      "Compression=1 未压缩，SubIFD 内嵌 JPEG 预览）。\n"
      "- 索引层（photos 表）的 `format` / `camera` 等元数据仍按真实基准标注"
      "（DC-S5M2 / RW2 占 92.2%），复刻真实图库的分布，供检索 / 筛选 / 统计验证。\n"
      "- **接真实图库时 LibRaw 直接解码 RW2，引擎代码无需任何改动**"
      "（RawDecoder 对 RW2 / DNG 走同一条 open_file -> unpack -> dcraw_process 通路）。\n"
      "\n"
      "## 分布基准（v1.0.0）\n"
      "\n"
      "- 总条目 18,192；5 个库根 / 79 个文件夹；月份范围 2025-12 ~ 2026-08\n"
      "- 缺失（missing）421；已调整 4,347（其中 83.8% 含局部蒙版）；评分 2 星 1,365 / "
      "3 星 1 / 4 星 57；带 GPS 137\n"
      "\n"
      "## 自检\n"
      "\n"
      "生成完成后自动执行 `raw.selfTest`（LibRaw 真解码抽样，要求 100% 成功且输出均值"
      " ∈ [16,240]），结果见 `mock.status` 的 `lastSelfTest`。\n";
  std::string err;
  writeFileAtomic(joinPath(libraryRoot, "README.md"), kReadme, &err);
}

void MockGenerator::run(const MockSpec& spec, JobContext& ctx) {
  int64_t startedMs = nowMs();
  embedPreview_ = spec.embedPreview;
  std::string root = spec.libraryRoot;
  if (root.empty()) {
    root = config_->settings().get("mockLibraryRoot").asString();
  }
  if (root.empty()) {
    ctx.progress(100, "mock library root is not configured");
    return;
  }
  logInfo("mock", "generate start: total=" + std::to_string(spec.total) + " root=" + root);
  if (!makeDirs(root)) {
    ctx.progress(100, "cannot create mock library root: " + root);
    return;
  }
  writeMockReadme(root);

  // ---- 1. 5 个库根（真实基准题材） ----
  // 目录名必须是纯 ASCII：宿主安全策略会拦截中文路径写入。
  // 中文题材名保留在 roots.label（DB 展示 / FTS5 检索仍为中文，不影响检索验证）。
  static const char* kRootLabels[] = {"风光-川西", "风光-滇藏", "风光-甘南", "城市-建筑", "延时-星空"};
  static const char* kRootSlugs[] = {"fg-chuanxi", "fg-dianzang", "fg-gannan", "cs-jianzhu",
                                     "ys-xingkong"};
  std::vector<int64_t> rootIds;
  std::vector<std::string> rootPaths;
  for (int i = 0; i < spec.rootCount; ++i) {
    std::string label = kRootLabels[i % 5];
    std::string slug = kRootSlugs[i % 5];
    std::string path = joinPath(root, padNumber(i + 1, 2) + "-" + slug);
    makeDirs(path);
    std::string err;
    rootIds.push_back(photos_->ensureRoot(path, label, &err));
    rootPaths.push_back(path);
  }
  logInfo("mock", "stage1 roots done: " + std::to_string(rootIds.size()));

  // ---- 2. 79 个文件夹（目录 ASCII，地点中文名保存在 DB 字段供 FTS5 中文检索） ----
  static const char* kPlaces[] = {"四姑娘山", "松赞林寺", "雾浓顶", "新都桥", "牛背山",
                                  "冷嘎措",   "色达",     "亚丁",   "措普沟", "莲宝叶则"};
  static const char* kPlaceSlugs[] = {"siguniangshan", "songzanlinsi", "wunongding", "xinduqiao",
                                      "niubeishan",    "lenggacuo",    "seda",       "yading",
                                      "cuopugou",      "lianbaoyueze"};
  struct FolderEntry {
    int64_t folderId = 0;
    int64_t rootId = 0;
    std::string rootPath;
    std::string relPath;
    std::string place;
  };
  std::vector<FolderEntry> folders;
  for (int i = 0; i < spec.folderCount; ++i) {
    int rootIdx = i % spec.rootCount;
    std::string place = kPlaces[i % 10];
    std::string slug = kPlaceSlugs[i % 10];
    std::string rel = slug + "/" + padNumber(i / 10 + 1, 2) + "-" + slug;
    std::string path = joinPath(rootPaths[static_cast<size_t>(rootIdx)], rel);
    makeDirs(path);
    std::string err;
    FolderEntry entry;
    entry.rootId = rootIds[static_cast<size_t>(rootIdx)];
    entry.rootPath = rootPaths[static_cast<size_t>(rootIdx)];
    entry.relPath = rel;
    entry.place = place;
    entry.folderId = photos_->ensureFolder(entry.rootId, rel, baseName(rel), &err);
    folders.push_back(entry);
  }
  logInfo("mock", "stage2 folders done: " + std::to_string(folders.size()));

  // ---- 3. 月份范围 ----
  int yearFrom = 0, monFrom = 0, yearTo = 0, monTo = 0;
  sscanf(spec.monthFrom.c_str(), "%d-%d", &yearFrom, &monFrom);
  sscanf(spec.monthTo.c_str(), "%d-%d", &yearTo, &monTo);
  if (yearFrom <= 0 || monFrom <= 0 || yearTo <= 0 || monTo <= 0) {
    yearFrom = 2025;
    monFrom = 12;
    yearTo = 2026;
    monTo = 8;
  }
  int monthCount = (yearTo - yearFrom) * 12 + (monTo - monFrom) + 1;
  if (monthCount < 1) monthCount = 1;

  int64_t filesWritten = 0;
  int64_t sidecarsWritten = 0;
  int64_t dbWritten = 0;
  int64_t diskBytes = 0;
  int64_t bracketsWritten = 0;  // 实际落盘成功的包围组数（写进 mock_meta，mock.status 读取）
  int64_t rw2Count = static_cast<int64_t>(spec.total * spec.rw2Ratio);
  if (rw2Count > spec.total) rw2Count = spec.total;
  int64_t maskCount = static_cast<int64_t>(spec.edited * spec.maskRatio);
  // 精确控制缺失张数：用比例边界法而不是 i % (total/missing)。
  // 后者会因整除舍入产生偏差（18192/421=43 → 实际命中 424 张，与报告值不一致）。
  const int64_t missingTotal = spec.missing > 0 ? (spec.missing > spec.total ? spec.total : spec.missing) : 0;

  // ---- 包围连拍预计算：bracketGroups 组 × 3 张相邻照片（i, i+1, i+2） ----
  // 识别条件（specialty/hdr_service.h 的 BracketScanOptions）：相邻帧间隔 ≤3s 且 EV 差 ≥1 档。
  // 因此每组约定：captured_at 间隔 1 秒；shutter 固定 1/500s / 1/125s / 1/30s（约 2EV 阶梯）；
  // aperture / iso 组内一致（取组起点照片的自然分布值）。普通相邻照片的拍摄间隔约 11 分钟，
  // 远大于 3 秒，包围链不会与普通照片误合并。
  constexpr int64_t kBracketFrames = 3;
  const double kBracketShutters[kBracketFrames] = {1.0 / 500.0, 1.0 / 125.0, 1.0 / 30.0};
  int64_t bracketGroupTarget = spec.bracketGroups;
  if (bracketGroupTarget > spec.total / kBracketFrames) {
    bracketGroupTarget = spec.total / kBracketFrames;  // 总量太小则按能容纳的组数截断
  }
  // bracketSlot[i]：i 在组内的序号（0/1/2），非包围帧为 -1；bracketBaseMs 只在 slot=0 处生效
  std::vector<int8_t> bracketSlot(static_cast<size_t>(spec.total), -1);
  std::vector<int64_t> bracketBaseMs(static_cast<size_t>(spec.total), 0);
  int64_t bracketsPlanned = 0;
  if (bracketGroupTarget > 0) {
    // 缺失判定复用主循环的比例边界法：isMissingIdx 仅依赖下标，可预先求值以挑选合法组起点
    auto isMissingIdx = [&](int64_t idx) -> bool {
      return missingTotal > 0 &&
             ((idx * missingTotal) / spec.total != ((idx + 1) * missingTotal) / spec.total);
    };
    // 与主循环同一套时间公式（月份 ×131 / 日 ×17 / 时 ×7 / 分 ×11 / 秒 ×13），
    // 组基准时刻 = 组起点照片的自然拍摄时间（秒归 0 后组内按 1 秒步进）
    auto naturalMs = [&](int64_t idx) -> int64_t {
      int monthIndex = static_cast<int>((idx * 131) % monthCount);
      int y = yearFrom + (monFrom - 1 + monthIndex) / 12;
      int m = (monFrom - 1 + monthIndex) % 12 + 1;
      char buf[32];
      snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d", y, m,
               static_cast<int>((idx * 17) % 28) + 1, static_cast<int>((idx * 7) % 24),
               static_cast<int>((idx * 11) % 60), 0);
      return parseMsFromDate(std::string(buf));
    };
    int64_t prevEnd = -1;  // 上一组占用的最大下标（保证组间互不重叠）
    for (int64_t g = 0; g < bracketGroupTarget; ++g) {
      // 组起点均匀铺开（第 g+1 等分点），向后找第一个合法起点：
      // 三张都落在 RAW 区（i < rw2Count）、都不缺失、且不与上一组重叠
      int64_t start = (g + 1) * spec.total / (bracketGroupTarget + 1);
      while (start + kBracketFrames <= rw2Count &&
             (start <= prevEnd + 1 || isMissingIdx(start) || isMissingIdx(start + 1) ||
              isMissingIdx(start + 2))) {
        ++start;
      }
      if (start + kBracketFrames > rw2Count) break;  // RAW 区放不下了，提前收尾
      for (int64_t k = 0; k < kBracketFrames; ++k) {
        bracketSlot[static_cast<size_t>(start + k)] = static_cast<int8_t>(k);
      }
      bracketBaseMs[static_cast<size_t>(start)] = naturalMs(start);
      prevEnd = start + kBracketFrames - 1;
      ++bracketsPlanned;
    }
    logInfo("mock", "bracket groups planned: " + std::to_string(bracketsPlanned) + "/" +
                        std::to_string(spec.bracketGroups));
  }

  // ---- 4. 逐张生成 ----
  logInfo("mock", "stage3 start loop: total=" + std::to_string(spec.total) +
                      " folders=" + std::to_string(folders.size()) +
                      " months=" + std::to_string(monthCount) +
                      " edited=" + std::to_string(spec.edited));
  for (int64_t i = 0; i < spec.total; ++i) {
    if (ctx.isCancelled()) return;
    bool isRaw = i < rw2Count;
    const FolderEntry& folder = folders[static_cast<size_t>((i * 7919) % folders.size())];
    int monthIndex = static_cast<int>((i * 131) % monthCount);
    int y = yearFrom + (monFrom - 1 + monthIndex) / 12;
    int m = (monFrom - 1 + monthIndex) % 12 + 1;
    int day = static_cast<int>((i * 17) % 28) + 1;
    int hour = static_cast<int>((i * 7) % 24);
    int minute = static_cast<int>((i * 11) % 60);
    char dateBuf[32];
    snprintf(dateBuf, sizeof(dateBuf), "%04d-%02d-%02d %02d:%02d:%02d", y, m, day, hour, minute,
             static_cast<int>((i * 13) % 60));
    std::string dateStr(dateBuf);
    int64_t capturedMs = parseMsFromDate(dateStr);

    std::string ext = isRaw ? "dng" : "jpg";
    std::string fileName = "P" + padNumber(static_cast<int>(i + 1), 7) + "." + ext;
    std::string filePath = joinPath(folder.rootPath, fileName);
    // 比例边界法：当 (i*missing)/total 跨越整数时命中，累计恰好 missingTotal 张
    bool isMissing = missingTotal > 0 &&
                     ((i * missingTotal) / spec.total != ((i + 1) * missingTotal) / spec.total);
    int style = static_cast<int>(i % 5);
    uint32_t seed = 0x1000 + static_cast<uint32_t>(i) * 2654435761u;
    int iso = 100 + static_cast<int>((i * 37) % 12) * 100;
    double aperture = 1.4 + static_cast<double>(i % 9) * 0.5;
    double shutter = 1.0 / (30.0 + static_cast<double>(i % 40) * 7.0);
    int focal = 14 + static_cast<int>((i * 3) % 10) * 6;

    // 包围连拍帧覆盖：组内 captured_at 按 1 秒步进，shutter 固定 2EV 阶梯；
    // aperture / iso / focal 取组起点（baseIdx = i - slot）的自然分布值，保证组内一致。
    // dateStr 同步改写，使 DNG 的 DateTime 标签与 photos.captured_at 一致。
    const int8_t bracketSlotIdx = bracketSlot[static_cast<size_t>(i)];
    if (bracketSlotIdx >= 0) {
      const int64_t baseIdx = i - bracketSlotIdx;
      capturedMs = bracketBaseMs[static_cast<size_t>(baseIdx)] + bracketSlotIdx * 1000;
      dateStr = formatTimestamp(capturedMs).substr(0, 19);  // "YYYY-MM-DD HH:MM:SS"
      iso = 100 + static_cast<int>((baseIdx * 37) % 12) * 100;
      aperture = 1.4 + static_cast<double>(baseIdx % 9) * 0.5;
      shutter = kBracketShutters[bracketSlotIdx];
      focal = 14 + static_cast<int>((baseIdx * 3) % 10) * 6;
    }

    PhotoRecord p;
    p.folderId = folder.folderId;
    p.rootId = folder.rootId;
    p.fileName = fileName;
    p.filePath = filePath;
    p.format = isRaw ? "RW2" : "JPG";
    p.capturedAt = capturedMs;
    p.importedAt = nowMs();
    p.camera = "DC-S5M2";
    p.lens = (i % 3 == 0) ? "LUMIX S 20-60mm F3.5-5.6" : "LUMIX S PRO 24-70mm F2.8";
    // FTS5 外部内容表列：地点名（中文检索）+ 月份（时间轴分组键）
    p.location = folder.place;
    char monthBuf[8];
    snprintf(monthBuf, sizeof(monthBuf), "%04d-%02d", y, m);
    p.date = monthBuf;
    p.iso = iso;
    p.aperture = "f/" + std::to_string(aperture).substr(0, 3);
    p.shutter = "1/" + std::to_string(static_cast<int>(1.0 / shutter + 0.5)) + "s";
    p.focalLength = std::to_string(focal) + "mm";
    p.width = spec.imageWidth;
    p.height = spec.imageHeight;
    p.isVideo = false;
    p.pairKind = "single";
    p.health = "unchecked";
    p.sidecarPath = replaceExtension(filePath, "xmp");
    p.deleted = false;
    if (isMissing) {
      p.isMissing = true;
      p.health = "missing";
    }
    if (i < spec.gps) {
      p.hasGps = true;
      p.gpsLat = 30.5 + static_cast<double>(i % 7) * 0.05;
      p.gpsLng = 101.2 + static_cast<double>(i % 11) * 0.05;
      p.gpsAlt = 3200.0 + static_cast<double>(i % 13) * 50.0;
    }
    if (i < spec.rating4) p.rating = 4;
    else if (i < spec.rating4 + spec.rating3) p.rating = 3;
    else if (i < spec.rating4 + spec.rating3 + spec.rating2) p.rating = 2;
    else p.rating = 0;

    if (!isMissing) {
      bool ok = false;
      if (isRaw) {
        ok = writeLinearDng(filePath, spec.imageWidth, spec.imageHeight, style, seed, dateStr, iso,
                            aperture, shutter, focal);
      } else {
        ok = writeJpegAsset(filePath, spec.imageWidth, spec.imageHeight, style, seed);
      }
      if (ok) {
        ++filesWritten;
        p.fileSize = fileSize(filePath);
        diskBytes += p.fileSize;
        p.health = "ok";
        // 以 slot 0 帧落盘成功记为一组（识别层靠三帧联动，首帧缺失即整组不可用）
        if (bracketSlotIdx == 0) ++bracketsWritten;
      } else {
        p.isMissing = true;
        p.health = "missing";
        logError("mock", "asset write failed: " + filePath);
      }
    }

    int64_t photoId = 0;
    std::string err;
    if (photos_->upsert(p, &photoId, &err)) {
      ++dbWritten;
      photos_->setHealth(photoId, p.health, &err);
      if (i < spec.edited) {
        json::JsonValue params = xmp::XmpMapper::defaultParams();
        params.setItem("exposure", (static_cast<double>(i % 21) - 10.0) * 0.1);
        params.setItem("contrast", (static_cast<double>(i % 41) - 20.0));
        params.setItem("highlights", -static_cast<double>(i % 31));
        params.setItem("shadows", static_cast<double>(i % 27));
        params.setItem("whiteBalance", (i % 4 == 0) ? "custom" : "asShot");
        if (i % 4 == 0) {
          params.setItem("temperature", 5200 + (i % 9) * 40);
          params.setItem("tint", (i % 11) - 5);
        }
        if (i < maskCount) {
          json::JsonValue masks = json::JsonValue::arrayValue();
          json::JsonValue mask = json::JsonValue::objectValue();
          mask.setItem("id", "mask-" + std::to_string(i));
          mask.setItem("type", (i % 3 == 0) ? "radial" : (i % 3 == 1 ? "linear" : "brush"));
          mask.setItem("enabled", true);
          mask.setItem("opacity", 80.0 + static_cast<double>(i % 5) * 4.0);
          mask.setItem("amount", (static_cast<double>(i % 21) - 10.0) * 6.0);
          json::JsonValue local = json::JsonValue::objectValue();
          local.setItem("exposure", (static_cast<double>(i % 9) - 4.0) * 0.2);
          local.setItem("contrast", static_cast<double>(i % 7) * 3.0);
          local.setItem("highlights", -static_cast<double>(i % 13) * 2.0);
          local.setItem("shadows", static_cast<double>(i % 11) * 3.0);
          local.setItem("temperature", (i % 9) - 4);
          local.setItem("tint", (i % 5) - 2);
          local.setItem("saturation", (i % 15) - 7);
          local.setItem("clarity", static_cast<double>(i % 9) * 2.0);
          local.setItem("dehaze", static_cast<double>(i % 7) * 3.0);
          mask.setItem("local", local);
          masks.addItem(mask);
          params.setItem("masks", masks);
        }
        std::string saveErr;
        // force=true：这是软件自己在重建模拟图库，不是用户手改 sidecar。
        // 若走外部修改检测，重复执行 mock.generate 会被自己的旧文件拦住（曾导致 3,471 次误拒）。
        if (xmp_->saveParams(photoId, params, &saveErr, /*force=*/true)) {
          ++sidecarsWritten;
        } else {
          logWarn("mock", "sidecar write failed for photo " + std::to_string(photoId) + ": " +
                              saveErr);
        }
      }
    } else {
      logWarn("mock", "db upsert failed for " + filePath + ": " + err);
    }

    if ((i % 2000) == 0) {
      int percent = static_cast<int>(i * 100 / spec.total);
      ctx.progress(percent, "generating " + std::to_string(i) + "/" + std::to_string(spec.total));
    }
  }

  // ---- 5. RAW+JPG 配对：为前 pairCount 张 RAW 生成同名 JPG 兄弟文件（一条目） ----
  int64_t pairs = 0;
  {
    std::string sql = "SELECT id, file_path, captured_at FROM photos WHERE format='RW2' AND "
                      "is_missing=0 AND deleted=0 ORDER BY id LIMIT ?;";
    auto stmt = db_->prepareCached("mockPairs", sql, nullptr);
    if (stmt && stmt->valid()) {
      stmt->bindInt64(1, spec.pairCount);
      while (stmt->step(nullptr)) {
        int64_t pairPhotoId = stmt->columnInt64(0);
        std::string rawPath = stmt->columnText(1);
        std::string jpgPath = replaceExtension(rawPath, "jpg");
        if (writeJpegAsset(jpgPath, spec.imageWidth, spec.imageHeight,
                           static_cast<int>(pairs % 5), 0x2000 + static_cast<uint32_t>(pairs))) {
          ++pairs;
          photos_->setPairKind(pairPhotoId, "raw+jpg", nullptr);
        }
      }
      stmt->reset();
    }
  }

  // ---- 6. 延时序列组（LRT 兼容）：1 组 198 帧，首帧写 lrt: 关键帧 ----
  for (int g = 0; g < spec.lrtGroups; ++g) {
    std::string seqName = "延时-雾浓顶-" + std::to_string(g + 1);
    std::string sql =
        "INSERT INTO sequences(name,kind,frame_count,state,created_at) VALUES(?,?,?,'idle',?);";
    auto stmt = db_->prepareCached("mockSequence", sql, nullptr);
    if (!stmt || !stmt->valid()) continue;
    stmt->bindText(1, seqName);
    stmt->bindText(2, "timelapse");
    stmt->bindInt64(3, 198);
    stmt->bindInt64(4, nowMs());
    stmt->step(nullptr);
    int64_t seqId = db_->lastInsertId();
    stmt->reset();

    std::string idsSql =
        "SELECT id FROM photos WHERE deleted=0 AND is_missing=0 ORDER BY captured_at LIMIT 198;";
    auto ids = db_->prepareCached("mockSeqFrames", idsSql, nullptr);
    if (ids && ids->valid()) {
      int frameNo = 1;
      while (ids->step(nullptr)) {
        int64_t photoId = ids->columnInt64(0);
        auto link = db_->prepareCached("mockSeqLink",
                                       "INSERT OR IGNORE INTO sequence_frames(seq_id,frame_no,"
                                       "photo_id) VALUES(?,?,?);",
                                       nullptr);
        if (link && link->valid()) {
          link->bindInt64(1, seqId);
          link->bindInt64(2, frameNo);
          link->bindInt64(3, photoId);
          link->step(nullptr);
          link->reset();
        }
        photos_->setSeqId(photoId, seqId, nullptr);
        ++frameNo;
      }
      ids->reset();
    }
    // 首帧 sidecar 尽力回写 lrt:（映射表 §6：LRTimelapse 可读）
    std::string lrtSidecar;
    int64_t lrtPhotoId = 0;
    auto first = db_->prepareCached("mockSeqFirst",
                                    "SELECT p.id, p.sidecar_path FROM photos p WHERE p.deleted=0 "
                                    "ORDER BY p.captured_at LIMIT 1;",
                                    nullptr);
    if (first && first->valid() && first->step(nullptr)) {
      lrtPhotoId = first->columnInt64(0);
      lrtSidecar = first->columnText(1);
    }
    if (first) first->reset();
    if (!lrtSidecar.empty() && fileExists(lrtSidecar)) {
      std::string bytes;
      std::string readErr;
      if (readFile(lrtSidecar, &bytes, &readErr)) {
        std::string parseErr;
        std::unique_ptr<xmp::XmpDocument> doc = xmp::XmpDocument::parse(bytes, &parseErr);
        if (doc) {
          doc->setText("lrt", "Keyframes", "0,0,5600 50,0.35,5750 100,0.70,5900 150,0.35,5600 197,0,5500");
          doc->setText("lrt", "Ramp", "Linear");
          std::string outErr;
          if (writeFileAtomic(lrtSidecar, doc->serialize(), &outErr)) {
            // 必须把新的 mtime 同步回 DB。漏了这一步，下一次 saveParams 的
            // 「sidecar 被外部修改」保护会把自己刚写的盘当成外部改动，
            // 用户在这张照片上第一次保存就撞 1005 冲突（曾导致冒烟整条修图链失败）。
            if (lrtPhotoId > 0) {
              std::string dbErr;
              photos_->setSidecar(lrtPhotoId, lrtSidecar, fileMtimeMs(lrtSidecar), true, &dbErr);
            }
          } else {
            logWarn("mock", "lrt ramp writeback failed for " + lrtSidecar + ": " + outErr);
          }
        }
      }
    }
  }

  // ---- 7. 重建 FTS 索引（保证中文检索可用） ----
  std::string ftsErr;
  fts_->rebuild(&ftsErr);

  // ---- 8. L1 自检：抽样走 LibRaw 真解码生成的 Linear DNG（v1.0.1 新增 dngSelfTest） ----
  raw::SelfTestResult selfTest;
  std::string selfErr;
  const int kSelfTestSample = 8;
  bool selfTestRan = decoder_->selfTest(photos_, kSelfTestSample, std::vector<int64_t>(),
                                        &selfTest, &selfErr);
  if (!selfTestRan) {
    logError("mock", "dng self-test could not run: " + selfErr);
  }
  json::JsonValue dngSelfTest = json::JsonValue::objectValue();
  dngSelfTest.setItem("ok", selfTestRan && selfTest.ok);
  dngSelfTest.setItem("sampled", selfTest.sampled);
  dngSelfTest.setItem("succeeded", selfTest.succeeded);
  dngSelfTest.setItem("decodeMsP50", selfTest.decodeMsP50);

  int64_t durationMs = nowMs() - startedMs;
  auto meta = db_->prepareCached("mockMeta",
                                 "INSERT INTO mock_meta(key,value) VALUES('dbPhotos',?) "
                                 "ON CONFLICT(key) DO UPDATE SET value=excluded.value;",
                                 nullptr);
  if (meta && meta->valid()) {
    meta->bindText(1, std::to_string(dbWritten));
    meta->step(nullptr);
    meta->reset();
  }
  // 记录最近一次自检结果（mock.status 的 lastSelfTest）
  {
    json::JsonValue last = json::JsonValue::objectValue();
    last.setItem("ok", selfTestRan && selfTest.ok);
    last.setItem("sampled", selfTest.sampled);
    last.setItem("succeeded", selfTest.succeeded);
    last.setItem("decodeMsP50", selfTest.decodeMsP50);
    last.setItem("decodeMsP95", selfTest.decodeMsP95);
    last.setItem("at", nowMs());
    auto st = db_->prepareCached("mockMetaSelfTest",
                                 "INSERT INTO mock_meta(key,value) VALUES('lastSelfTest',?) "
                                 "ON CONFLICT(key) DO UPDATE SET value=excluded.value;",
                                 nullptr);
    if (st && st->valid()) {
      st->bindText(1, last.serialize());
      st->step(nullptr);
      st->reset();
    }
  }
  // 记录实际落盘成功的包围连拍组数（mock.status 的 bracketGroups）
  {
    auto bg = db_->prepareCached("mockMetaBrackets",
                                 "INSERT INTO mock_meta(key,value) VALUES('bracketGroups',?) "
                                 "ON CONFLICT(key) DO UPDATE SET value=excluded.value;",
                                 nullptr);
    if (bg && bg->valid()) {
      bg->bindText(1, std::to_string(bracketsWritten));
      bg->step(nullptr);
      bg->reset();
    }
  }

  // ---- 9. 汇总（ipc-protocol v1.0.1 §5.15 完成 result） ----
  json::JsonValue result = json::JsonValue::objectValue();
  result.setItem("dbPhotos", dbWritten);
  result.setItem("imageFiles", filesWritten);
  result.setItem("sidecars", sidecarsWritten);
  result.setItem("missing", spec.missing);
  result.setItem("diskBytes", diskBytes);
  result.setItem("bracketGroups", bracketsWritten);
  result.setItem("durationMs", durationMs);
  result.setItem("dngSelfTest", dngSelfTest);
  result.setItem("readmePath", joinPath(root, "README.md"));
  ctx.setResult(result);
  ctx.progress(100, "mock library generated: " + std::to_string(dbWritten) + " records in " +
                        std::to_string(durationMs) + "ms");
  logInfo("mock", "generated " + std::to_string(dbWritten) + " records, " +
                      std::to_string(filesWritten) + " files, " + std::to_string(sidecarsWritten) +
                      " sidecars, " + std::to_string(diskBytes) + " bytes, self-test ok=" +
                      (selfTestRan && selfTest.ok ? "true" : "false") + " in " +
                      std::to_string(durationMs) + "ms");
}

json::JsonValue MockGenerator::status(rpc::RpcError* err) {
  json::JsonValue out = json::JsonValue::objectValue();
  std::string sqlErr;
  // generated 的判定必须来自"模拟图库自己写下的账"（mock_meta.dbPhotos），
  // 不能用 photos 计数代理：真实图库一导入照片，就再也不能是 generated=false。
  int64_t mockPhotos = 0;
  {
    auto stmt = db_->prepareCached("mockStatus", "SELECT value FROM mock_meta WHERE key='dbPhotos';",
                                   &sqlErr);
    if (stmt && stmt->valid()) {
      if (stmt->step(&sqlErr)) mockPhotos = stmt->columnInt64(0);
      stmt->reset();
    }
  }
  // dbPhotoCount 是"库里现在实际有多少张"（真实图库也适用），与 generated 分开
  int64_t dbPhotos = 0;
  {
    auto cs = db_->prepareCached("mockStatusCount", "SELECT COUNT(*) FROM photos WHERE deleted=0;",
                                 &sqlErr);
    if (cs && cs->valid()) {
      if (cs->step(&sqlErr)) dbPhotos = cs->columnInt64(0);
      cs->reset();
    }
  }
  // 最近一次自检结果（raw.selfTest / dngSelfTest 同源）
  json::JsonValue lastSelfTest = json::JsonValue::nullValue();
  auto ls = db_->prepareCached("mockStatusSelfTest",
                               "SELECT value FROM mock_meta WHERE key='lastSelfTest';", &sqlErr);
  if (ls && ls->valid()) {
    if (ls->step(&sqlErr)) {
      json::JsonValue parsed;
      std::string parseErr;
      if (json::JsonValue::parse(ls->columnText(0), &parsed, &parseErr)) {
        lastSelfTest = parsed;
      }
    }
    ls->reset();
  }
  // 包围连拍组数（生成阶段实际落盘成功的组数，写进 mock_meta['bracketGroups']）
  int64_t bracketGroups = 0;
  {
    auto bs = db_->prepareCached("mockStatusBrackets",
                                 "SELECT value FROM mock_meta WHERE key='bracketGroups';", &sqlErr);
    if (bs && bs->valid()) {
      if (bs->step(&sqlErr)) bracketGroups = bs->columnInt64(0);
      bs->reset();
    }
  }
  out.setItem("generated", mockPhotos > 0);
  out.setItem("libraryRoot", config_->settings().get("mockLibraryRoot").asString());
  out.setItem("dbPhotoCount", dbPhotos);
  out.setItem("mockPhotoCount", mockPhotos);
  out.setItem("bracketGroups", bracketGroups);
  out.setItem("assetFormat", "linear-dng");
  out.setItem("lastSelfTest", lastSelfTest);
  return out;
}

}  // namespace mock
}  // namespace lumina
