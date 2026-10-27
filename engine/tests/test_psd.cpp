// M3-01 测试：PSD 写出/读取（PackBits、8·16bit、Raw/RLE、图层属性、lumi 快照、
// 未知 tagged block「读→写」字节一致、外来 PSD 无快照判定）
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "export/psd_reader.h"
#include "export/psd_writer.h"

using namespace lumina::psd;  // NOLINT(build/namespaces)

static int g_fail = 0;
#define CHECK(cond, msg)                                       \
  do {                                                         \
    if (!(cond)) {                                             \
      std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, msg);  \
      ++g_fail;                                                \
    }                                                          \
  } while (0)

// ctest 的工作目录就是 build/engine，产物直接落在当前目录（与其他测试同口径）
static std::string tmpPath(const std::string& name) {
  return name;
}

// ---- 1. PackBits 编解码往返（含全同、全异、长度 1、跨 128 边界） ----
static void testPackBits() {
  struct Case {
    std::vector<uint8_t> src;
    const char* label;
  };
  std::vector<Case> cases;
  cases.push_back({{}, "empty"});
  cases.push_back({{7}, "single"});
  cases.push_back({{1, 1, 1, 1, 1}, "all-same-5"});
  cases.push_back({{0, 1, 2, 3, 4, 5}, "all-diff"});
  std::vector<uint8_t> longRun(300, 0xAB);
  cases.push_back({longRun, "run-300"});
  std::vector<uint8_t> longLiteral(300);
  for (size_t i = 0; i < longLiteral.size(); ++i) longLiteral[i] = static_cast<uint8_t>(i * 7 + 1);
  cases.push_back({longLiteral, "literal-300"});
  std::vector<uint8_t> mixed;
  for (int i = 0; i < 200; ++i) {
    mixed.push_back(static_cast<uint8_t>(i));
    mixed.push_back(static_cast<uint8_t>(i));
    mixed.push_back(static_cast<uint8_t>(i));
    mixed.push_back(static_cast<uint8_t>(i + 1));
  }
  cases.push_back({mixed, "mixed"});

  for (const Case& c : cases) {
    std::vector<uint8_t> packed;
    packBitsEncode(c.src.data(), static_cast<int>(c.src.size()), &packed);
    std::vector<uint8_t> out;
    const bool ok = packBitsDecode(packed.data(), static_cast<int>(packed.size()), &out,
                                   static_cast<int>(c.src.size()));
    CHECK(ok, (std::string("packBits decode: ") + c.label).c_str());
    CHECK(out == c.src, (std::string("packBits roundtrip: ") + c.label).c_str());
  }
}

// ---- 构造一个两层文档（带蒙版、lumi 快照、未知块） ----
static Document makeDoc(int w, int h, int bps, const std::vector<UnknownBlock>& unknown) {
  Document doc;
  for (int li = 0; li < 2; ++li) {
    Layer l;
    l.name = li == 0 ? "背景 · Basic" : "曲线 · Curve";
    l.top = 0;
    l.left = 0;
    l.width = w;
    l.height = h;
    l.opacity = li == 0 ? 255 : 200;
    l.visible = true;
    l.blendKey = li == 0 ? "normal" : "multiply";
    l.rgb.assign(static_cast<size_t>(w) * h * 3 * bps, 0);
    for (int y = 0; y < h; ++y) {
      for (int x = 0; x < w; ++x) {
        const size_t at = (static_cast<size_t>(y) * w + x) * 3 * bps;
        const int v = (x * 3 + y * 5 + li * 40) & 0xFF;
        if (bps == 1) {
          l.rgb[at] = static_cast<uint8_t>(v);
          l.rgb[at + 1] = static_cast<uint8_t>(255 - v);
          l.rgb[at + 2] = static_cast<uint8_t>((v * 2) & 0xFF);
        } else {
          const uint16_t v16 = static_cast<uint16_t>((v << 8) | v);
          l.rgb[at] = static_cast<uint8_t>(v16 >> 8);
          l.rgb[at + 1] = static_cast<uint8_t>(v16 & 0xFF);
          l.rgb[at + 2] = static_cast<uint8_t>((255 - v) >> 8);
          l.rgb[at + 3] = static_cast<uint8_t>((255 - v) & 0xFF);
          l.rgb[at + 4] = static_cast<uint8_t>(v16 >> 8);
          l.rgb[at + 5] = static_cast<uint8_t>(v16 & 0xFF);
        }
      }
    }
    if (li == 1) {
      l.mask.assign(static_cast<size_t>(w) * h * bps, 0);
      for (size_t i = 0; i < l.mask.size(); ++i) {
        l.mask[i] = bps == 1 ? static_cast<uint8_t>(i & 0xFF) : static_cast<uint8_t>((i * 3) & 0xFF);
      }
    }
    l.luminaSnapshot = std::string("{\"node\":\"") + (li == 0 ? "basic" : "curve") +
                       "\",\"params\":{\"exposure\":0.35}}";
    l.unknown = unknown;
    doc.layers.push_back(std::move(l));
  }
  doc.composite.assign(static_cast<size_t>(w) * h * 3 * bps, 0);
  for (size_t i = 0; i < doc.composite.size(); ++i) {
    const uint8_t v = static_cast<uint8_t>((i * 11) & 0xFF);
    doc.composite[i] = v;
  }
  return doc;
}

// ---- 2. 写→读往返：结构、属性、像素、蒙版、快照逐项相等 ----
static void testRoundtrip(int depth, bool rle) {
  const int w = 37, h = 23;  // 质数尺寸：专门让 RLE 行/通道边界错不开
  const int bps = depth == 16 ? 2 : 1;
  std::vector<UnknownBlock> unknown;
  UnknownBlock ub;
  ub.signature = "8BIM";
  ub.key = "lyid";
  ub.data = {0x00, 0x00, 0x00, 0x07};
  unknown.push_back(ub);
  const Document doc = makeDoc(w, h, bps, unknown);
  WriteOptions opts;
  opts.width = w;
  opts.height = h;
  opts.depth = depth;
  opts.rle = rle;
  const std::string path = tmpPath(std::string("m3-psd-rt-") + std::to_string(depth) + (rle ? "-rle" : "-raw") + ".psd");
  std::string err;
  const bool wrote = writePsd(path, doc, opts, &err);
  CHECK(wrote, (std::string("writePsd (depth=" + std::to_string(depth) + (rle ? " rle" : " raw") + "): " + err).c_str()));
  if (!wrote) return;

  PsdInfo info;
  std::string rerr;
  const bool read = readPsd(path, &info, &rerr);
  CHECK(read, (std::string("readPsd: ") + rerr).c_str());
  if (!read) return;

  CHECK(info.width == w && info.height == h, "画幅往返");
  CHECK(info.depth == depth, "位深往返");
  CHECK(info.layers.size() == 2, "层数往返");
  if (info.layers.size() != 2) return;
  CHECK(info.hasLuminaSnapshot, "lumi 快照被识别");
  CHECK(info.layers[0].name == doc.layers[0].name, "层名往返（含非 ASCII）");
  CHECK(info.layers[1].name == doc.layers[1].name, "层名往返 2");
  CHECK(info.layers[0].opacity == 255 && info.layers[1].opacity == 200, "不透明度往返");
  CHECK(info.layers[0].visible && info.layers[1].visible, "可见性往返");
  CHECK(info.layers[0].blendKey == "normal" && info.layers[1].blendKey == "multiply",
        "混合模式往返");
  CHECK(info.layers[0].rgb == doc.layers[0].rgb, "底层像素逐字节往返");
  CHECK(info.layers[1].rgb == doc.layers[1].rgb, "顶层像素逐字节往返");
  CHECK(info.layers[1].mask == doc.layers[1].mask, "蒙版逐字节往返");
  CHECK(info.layers[0].mask.empty(), "无蒙版层不应有蒙版");
  // 快照写入时补空格到 4 的倍数，读回需按此比对
  const std::string want0 = doc.layers[0].luminaSnapshot;
  CHECK(info.layers[0].luminaSnapshot.compare(0, want0.size(), want0) == 0, "快照内容往返");
  CHECK(info.composite == doc.composite, "合成图逐字节往返");
  CHECK(info.unknownBlockCount == 2, "未知块数量（每层 1 个）");
  CHECK(info.layers[0].unknown.size() == 1 && info.layers[0].unknown[0].key == "lyid",
        "未知块 key 往返");
  CHECK(info.layers[0].unknown[0].data == ub.data, "未知块数据逐字节往返");
}

// ---- 3. 未知块「读 → 写」字节一致（M3-01 判据③） ----
static void testUnknownBlockByteIdentical() {
  // 第一轮：写带未知块的 PSD（覆盖奇偶长度，专门打 padding 规则）
  std::vector<UnknownBlock> unknown;
  for (int n = 1; n <= 9; ++n) {
    UnknownBlock ub;
    ub.signature = "8BIM";
    ub.key = "unkn";
    ub.data.assign(static_cast<size_t>(n), static_cast<uint8_t>(0xA0 + n));
    unknown.push_back(ub);
  }
  const Document doc = makeDoc(17, 11, 1, unknown);
  WriteOptions opts;
  opts.width = 17;
  opts.height = 11;
  opts.depth = 8;
  opts.rle = true;
  const std::string p1 = tmpPath(std::string("m3-psd-unknown-1.psd"));
  const std::string p2 = tmpPath(std::string("m3-psd-unknown-2.psd"));
  std::string err;
  CHECK(writePsd(p1, doc, opts, &err), (std::string("写第一轮: ") + err).c_str());

  PsdInfo info;
  std::string rerr;
  CHECK(readPsd(p1, &info, &rerr), (std::string("读第一轮: ") + rerr).c_str());
  CHECK(info.unknownBlockCount == 18, "未知块总数（9 个 × 2 层）");

  // 第二轮：把读出来的未知块原样写回
  Document doc2;
  doc2.composite = doc.composite;
  for (size_t li = 0; li < info.layers.size(); ++li) {
    Layer l;
    l.name = info.layers[li].name;
    l.top = info.layers[li].top;
    l.left = info.layers[li].left;
    l.width = info.layers[li].width;
    l.height = info.layers[li].height;
    l.opacity = info.layers[li].opacity;
    l.visible = info.layers[li].visible;
    l.blendKey = info.layers[li].blendKey;
    l.rgb = info.layers[li].rgb;
    l.mask = info.layers[li].mask;
    l.luminaSnapshot.clear();  // 第二轮不写快照：验证「没有就不写」
    l.unknown = info.layers[li].unknown;
    doc2.layers.push_back(std::move(l));
  }
  CHECK(writePsd(p2, doc2, opts, &err), (std::string("写第二轮: ") + err).c_str());

  // 第三轮：读第二轮文件，未知块必须与第一轮逐字节相同
  PsdInfo info2;
  CHECK(readPsd(p2, &info2, &rerr), (std::string("读第二轮: ") + rerr).c_str());
  CHECK(info2.unknownBlockCount == info.unknownBlockCount, "未知块数量二次一致");
  CHECK(!info2.hasLuminaSnapshot, "第二轮无 lumi 快照 → 判为外来文件");
  bool same = info2.layers.size() == info.layers.size();
  for (size_t li = 0; same && li < info2.layers.size(); ++li) {
    same = info2.layers[li].unknown.size() == info.layers[li].unknown.size();
    for (size_t bi = 0; same && bi < info2.layers[li].unknown.size(); ++bi) {
      const UnknownBlock& a = info.layers[li].unknown[bi];
      const UnknownBlock& b = info2.layers[li].unknown[bi];
      same = a.signature == b.signature && a.key == b.key && a.data == b.data;
      if (!same) {
        std::printf("  块不一致: layer=%zu block=%zu key=%s len=%zu/%zu\n", li, bi, a.key.c_str(),
                    a.data.size(), b.data.size());
      }
    }
  }
  CHECK(same, "未知块「读→写→读」逐字节一致（含 padding 规则）");
}

// ---- 4. 入参不自洽必须报错，不写坏文件 ----
static void testRejects() {
  const Document doc = makeDoc(8, 8, 1, {});
  std::string err;
  WriteOptions bad = WriteOptions();
  bad.width = 8;
  bad.height = 8;
  bad.depth = 32;  // 不支持
  CHECK(!writePsd(tmpPath(std::string("m3-psd-bad.psd")), doc, bad, &err), "位深 32 必须拒绝");

  WriteOptions bad2;
  bad2.width = 8;
  bad2.height = 8;
  bad2.depth = 8;
  Document badDoc = doc;
  badDoc.composite.clear();  // 合成图长度不符
  CHECK(!writePsd(tmpPath(std::string("m3-psd-bad.psd")), badDoc, bad2, &err), "合成图长度不符必须拒绝");

  WriteOptions bad3;
  bad3.width = 40000;
  bad3.height = 8;
  bad3.depth = 8;
  CHECK(!writePsd(tmpPath(std::string("m3-psd-bad.psd")), doc, bad3, &err), "超 PSD 画幅上限必须拒绝");
}

int main() {
  testPackBits();
  testRoundtrip(8, true);
  testRoundtrip(8, false);
  testRoundtrip(16, true);
  testRoundtrip(16, false);
  testUnknownBlockByteIdentical();
  testRejects();
  if (g_fail == 0) {
    std::printf("test_psd: ALL PASS\n");
    return 0;
  }
  std::printf("test_psd: %d FAILED\n", g_fail);
  return 1;
}
