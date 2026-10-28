// A2：导出确定性常驻回归测试（守护 v1.2.1 OI-1 修复）
//
// 背景：v1.2.1 之前，导出物里的 ICC profile 字节因 lcms2 把构造时刻写入 ICC 头
// [24:36] 而不跨次/跨进程一致；修复方式是在 color/icc.cpp 的 profileBytes 里把
// 该字段归一化为 2000-01-01。此前确定性只靠脚本 scripts/verify_icc_determinism.py
// （单张样张 ×2 次）手动验证，现在做成常驻 ctest，防止回归。
//
// 判据：
//   ① 对每个合成变体 × 4 种格式（PNG/JPEG/TIFF/PSD），跑**真实的**生产编码+嵌入
//      路径**两次**（分别写到两个不同临时文件），逐字节比对两个产物必须完全一致。
//   ② 显式锁定 OI-1 回归：同进程内 profileBytes(sRGB) 调两次 → 逐字节一致；
//      AdobeRGB 同样；并且 sRGB != AdobeRGB。这直接守护已修复的 bug 不再回退。
//
// 设计约束：仅依赖 std + 引擎头，不引入第三方库；临时文件写到 cwd（ctest cwd 为
// build 目录），比对后 std::remove 清理；每个 (变体,格式) 打印一条 PASS/FAIL，
// 全局失败计数器，main 在失败返回 1。
#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "color/icc.h"
#include "export/exporter.h"
#include "export/icc_embed.h"
#include "export/psd_writer.h"
#include "imaging/imaging.h"

namespace {

int g_fail = 0;

// CHECK 风格宏：失败打印 file:line（参考 test_output_encode.cpp）
#define CHECK(cond, msg)                                       \
  do {                                                         \
    if (!(cond)) {                                             \
      std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, msg); \
      ++g_fail;                                                \
    }                                                          \
  } while (0)

/** 读全文件到内存（rb） */
bool readFile(const std::string& path, std::vector<uint8_t>* out) {
  FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) return false;
  std::fseek(f, 0, SEEK_END);
  const long n = std::ftell(f);
  std::fseek(f, 0, SEEK_SET);
  if (n < 0) {
    std::fclose(f);
    return false;
  }
  out->resize(static_cast<size_t>(n));
  const size_t rd = std::fread(out->data(), 1, out->size(), f);
  std::fclose(f);
  return rd == out->size();
}

/**
 * 逐字节比对两个产物：长度一致 → memcmp；不一致时记录首个差异偏移 + 两端字节值
 * （不静默通过）。每个 (变体,格式) 打印一条 PASS/FAIL。
 */
void compareRuns(const std::string& run1, const std::string& run2, const std::string& label) {
  std::vector<uint8_t> a, b;
  if (!readFile(run1, &a) || !readFile(run2, &b)) {
    std::printf("FAIL %s: 读文件失败（%s vs %s）\n", label.c_str(), run1.c_str(), run2.c_str());
    ++g_fail;
    return;
  }
  if (a.size() != b.size()) {
    std::printf("FAIL %s: 长度不一致 run1=%zu run2=%zu\n", label.c_str(), a.size(), b.size());
    ++g_fail;
    return;
  }
  int firstDiff = -1;
  uint8_t va = 0, vb = 0;
  for (size_t i = 0; i < a.size(); ++i) {
    if (a[i] != b[i]) {
      firstDiff = static_cast<int>(i);
      va = a[i];
      vb = b[i];
      break;
    }
  }
  if (firstDiff < 0) {
    std::printf("PASS %s: 两次导出逐字节一致（%zu 字节）\n", label.c_str(), a.size());
  } else {
    std::printf("FAIL %s: 偏移 %d 字节不同 run1=0x%02X run2=0x%02X\n", label.c_str(), firstDiff,
                va, vb);
    ++g_fail;
  }
}

// ---------------------------------------------------------------------------
// 合成变体（自包含，不依赖外部样张/DB）
// ---------------------------------------------------------------------------

/** 16x16 水平渐变（R 随宽度 0..255，G/B=0） */
lumina::imaging::Image makeHGradient() {
  lumina::imaging::Image img;
  img.width = 16;
  img.height = 16;
  img.pixels.assign(static_cast<size_t>(img.width) * img.height * 3, 0);
  for (int y = 0; y < img.height; ++y) {
    for (int x = 0; x < img.width; ++x) {
      const size_t i = (static_cast<size_t>(y) * img.width + x) * 3;
      img.pixels[i] = static_cast<uint8_t>(x * 255 / (img.width - 1));
      img.pixels[i + 1] = 0;
      img.pixels[i + 2] = 0;
    }
  }
  return img;
}

/** 64x48 垂直渐变（G 随高度 0..255）+ 几块纯色 */
lumina::imaging::Image makeVGradientBlocks() {
  lumina::imaging::Image img;
  img.width = 64;
  img.height = 48;
  img.pixels.assign(static_cast<size_t>(img.width) * img.height * 3, 0);
  for (int y = 0; y < img.height; ++y) {
    for (int x = 0; x < img.width; ++x) {
      const size_t i = (static_cast<size_t>(y) * img.width + x) * 3;
      img.pixels[i] = 0;
      img.pixels[i + 1] = static_cast<uint8_t>(y * 255 / (img.height - 1));
      img.pixels[i + 2] = 0;
    }
  }
  // 左上角纯红块
  for (int y = 4; y < 12; ++y) {
    for (int x = 4; x < 14; ++x) {
      const size_t i = (static_cast<size_t>(y) * img.width + x) * 3;
      img.pixels[i] = 255;
      img.pixels[i + 1] = 0;
      img.pixels[i + 2] = 0;
    }
  }
  // 右下角纯蓝块
  for (int y = img.height - 12; y < img.height - 4; ++y) {
    for (int x = img.width - 14; x < img.width - 4; ++x) {
      const size_t i = (static_cast<size_t>(y) * img.width + x) * 3;
      img.pixels[i] = 0;
      img.pixels[i + 1] = 0;
      img.pixels[i + 2] = 255;
    }
  }
  return img;
}

/** 128x96 双灰棋盘 + 彩色对角线（为非平凡彩色，便于 AdobeRGB 路径覆盖） */
lumina::imaging::Image makeCheckerDiagonal() {
  lumina::imaging::Image img;
  img.width = 128;
  img.height = 96;
  img.pixels.assign(static_cast<size_t>(img.width) * img.height * 3, 0);
  const int cell = 8;
  for (int y = 0; y < img.height; ++y) {
    for (int x = 0; x < img.width; ++x) {
      const size_t i = (static_cast<size_t>(y) * img.width + x) * 3;
      const int cx = x / cell;
      const int cy = y / cell;
      const uint8_t g = ((cx + cy) & 1) ? 200 : 60;
      img.pixels[i] = g;
      img.pixels[i + 1] = g;
      img.pixels[i + 2] = g;
    }
  }
  // 彩色对角线（红→黄渐变），跨整图
  for (int t = 0; t < img.width; ++t) {
    const int x = t;
    const int y = (t * (img.height - 1)) / (img.width - 1);
    if (x >= 0 && x < img.width && y >= 0 && y < img.height) {
      const size_t i = (static_cast<size_t>(y) * img.width + x) * 3;
      const uint8_t r = 255;
      const uint8_t g = static_cast<uint8_t>(t * 255 / (img.width - 1));
      img.pixels[i] = r;
      img.pixels[i + 1] = g;
      img.pixels[i + 2] = 0;
    }
  }
  return img;
}

// ---------------------------------------------------------------------------
// 单变体 × 四格式：真实生产编码+嵌入路径跑两次，比对
// ---------------------------------------------------------------------------

void testVariantDeterminism(const std::string& vname, const lumina::imaging::Image& img,
                            const std::vector<uint8_t>& icc, const std::string& iccLabel) {
  const std::string tag = vname + "[" + iccLabel + "]";
  std::string err;

  // ---- PNG：encodePng → injectPngIcc（run1/run2 各自重新 encode 后各注入一次） ----
  {
    const std::string p1 = "det_" + vname + "_png_1.png";
    const std::string p2 = "det_" + vname + "_png_2.png";
    std::remove(p1.c_str());
    std::remove(p2.c_str());
    CHECK(lumina::imaging::encodePng(p1, img, &err), ("png encode run1 " + tag + " " + err).c_str());
    CHECK(lumina::icc_embed::injectPngIcc(p1, icc, &err),
          ("png inject run1 " + tag + " " + err).c_str());
    CHECK(lumina::imaging::encodePng(p2, img, &err), ("png encode run2 " + tag + " " + err).c_str());
    CHECK(lumina::icc_embed::injectPngIcc(p2, icc, &err),
          ("png inject run2 " + tag + " " + err).c_str());
    compareRuns(p1, p2, tag + "/png");
    std::remove(p1.c_str());
    std::remove(p2.c_str());
  }

  // ---- JPEG：encodeJpeg(q=90) → injectJpegIcc ----
  {
    const std::string p1 = "det_" + vname + "_jpg_1.jpg";
    const std::string p2 = "det_" + vname + "_jpg_2.jpg";
    std::remove(p1.c_str());
    std::remove(p2.c_str());
    CHECK(lumina::imaging::encodeJpeg(p1, img, 90, &err),
          ("jpg encode run1 " + tag + " " + err).c_str());
    CHECK(lumina::icc_embed::injectJpegIcc(p1, icc, &err),
          ("jpg inject run1 " + tag + " " + err).c_str());
    CHECK(lumina::imaging::encodeJpeg(p2, img, 90, &err),
          ("jpg encode run2 " + tag + " " + err).c_str());
    CHECK(lumina::icc_embed::injectJpegIcc(p2, icc, &err),
          ("jpg inject run2 " + tag + " " + err).c_str());
    compareRuns(p1, p2, tag + "/jpg");
    std::remove(p1.c_str());
    std::remove(p2.c_str());
  }

  // ---- TIFF：Exporter::writeTiffForTest（与生产 writeTiff 同实现）调两次 ----
  {
    const std::string p1 = "det_" + vname + "_tif_1.tif";
    const std::string p2 = "det_" + vname + "_tif_2.tif";
    std::remove(p1.c_str());
    std::remove(p2.c_str());
    CHECK(lumina::exporter::Exporter::writeTiffForTest(p1, img, icc, &err),
          ("tif write run1 " + tag + " " + err).c_str());
    CHECK(lumina::exporter::Exporter::writeTiffForTest(p2, img, icc, &err),
          ("tif write run2 " + tag + " " + err).c_str());
    compareRuns(p1, p2, tag + "/tif");
    std::remove(p1.c_str());
    std::remove(p2.c_str());
  }

  // ---- PSD：Document{ 一个 Layer{rgb=img.pixels}, composite=img.pixels }，writePsd 两次 ----
  {
    const std::string p1 = "det_" + vname + "_psd_1.psd";
    const std::string p2 = "det_" + vname + "_psd_2.psd";
    std::remove(p1.c_str());
    std::remove(p2.c_str());
    lumina::psd::Document doc;
    lumina::psd::Layer L;
    L.name = "Layer";
    L.top = 0;
    L.left = 0;
    L.width = img.width;
    L.height = img.height;
    L.opacity = 255;
    L.visible = true;
    L.blendKey = "norm";
    L.rgb = img.pixels;  // w*h*3 (depth8)
    doc.layers.push_back(L);
    doc.composite = img.pixels;  // w*h*3
    lumina::psd::WriteOptions opts;
    opts.width = img.width;
    opts.height = img.height;
    opts.depth = 8;
    opts.rle = true;
    opts.writeComposite = true;
    opts.dpi = 72;
    opts.iccProfile = icc;
    CHECK(lumina::psd::writePsd(p1, doc, opts, &err),
          ("psd write run1 " + tag + " " + err).c_str());
    CHECK(lumina::psd::writePsd(p2, doc, opts, &err),
          ("psd write run2 " + tag + " " + err).c_str());
    compareRuns(p1, p2, tag + "/psd");
    std::remove(p1.c_str());
    std::remove(p2.c_str());
  }
}

// ---------------------------------------------------------------------------
// OI-1 显式回归锁：profileBytes 同进程内调两次逐字节一致；sRGB != AdobeRGB
// ---------------------------------------------------------------------------

void testProfileBytesDeterminism() {
  std::vector<uint8_t> srgb1, srgb2, adobe1, adobe2;
  std::string err;

  CHECK(lumina::color::profileBytes(lumina::color::ProfileId::Srgb, &srgb1, &err),
        ("profileBytes(sRGB) #1 " + err).c_str());
  CHECK(lumina::color::profileBytes(lumina::color::ProfileId::Srgb, &srgb2, &err),
        ("profileBytes(sRGB) #2 " + err).c_str());
  CHECK(!srgb1.empty(), "profileBytes(sRGB) 非空");
  CHECK(srgb1 == srgb2, "OI-1: profileBytes(sRGB) 同进程两次逐字节一致");

  CHECK(lumina::color::profileBytes(lumina::color::ProfileId::AdobeRgb, &adobe1, &err),
        ("profileBytes(AdobeRGB) #1 " + err).c_str());
  CHECK(lumina::color::profileBytes(lumina::color::ProfileId::AdobeRgb, &adobe2, &err),
        ("profileBytes(AdobeRGB) #2 " + err).c_str());
  CHECK(!adobe1.empty(), "profileBytes(AdobeRGB) 非空");
  CHECK(adobe1 == adobe2, "OI-1: profileBytes(AdobeRGB) 同进程两次逐字节一致");

  CHECK(srgb1 != adobe1, "OI-1: profileBytes(sRGB) 与 AdobeRGB 互异（未被误归一化）");

  if (srgb1 == srgb2 && adobe1 == adobe2 && srgb1 != adobe1) {
    std::printf("PASS OI-1: profileBytes 确定性 + sRGB/AdobeRGB 互异（守护已修复 bug 不回退）\n");
  } else {
    std::printf("FAIL OI-1: profileBytes 确定性回归\n");
    ++g_fail;
  }
}

}  // namespace

int main() {
  std::printf("== test_export_determinism (A2: 导出确定性 + OI-1 守护) ==\n");

  // 三个合成变体（不同尺寸 + 内容）
  const lumina::imaging::Image v1 = makeHGradient();       // 16x16  水平渐变
  const lumina::imaging::Image v2 = makeVGradientBlocks();  // 64x48  垂直渐变+纯色块
  const lumina::imaging::Image v3 = makeCheckerDiagonal();  // 128x96 棋盘+彩色对角线

  std::vector<uint8_t> srgb, adobe;
  std::string err;
  CHECK(lumina::color::profileBytes(lumina::color::ProfileId::Srgb, &srgb, &err),
        ("setup profileBytes(sRGB) " + err).c_str());
  CHECK(lumina::color::profileBytes(lumina::color::ProfileId::AdobeRgb, &adobe, &err),
        ("setup profileBytes(AdobeRGB) " + err).c_str());

  // 变体 1、2 走 sRGB；变体 3（非平凡彩色）走 AdobeRGB，验证两路径都确定且互异
  if (!srgb.empty()) {
    testVariantDeterminism("v1_hgrad", v1, srgb, "sRGB");
    testVariantDeterminism("v2_vgrad", v2, srgb, "sRGB");
  }
  if (!adobe.empty()) {
    testVariantDeterminism("v3_chkdiag", v3, adobe, "AdobeRGB");
  }

  // OI-1 显式回归锁
  testProfileBytesDeterminism();

  if (g_fail == 0) {
    std::printf("test_export_determinism: ALL PASS\n");
    return 0;
  }
  std::printf("test_export_determinism: %d FAILURES\n", g_fail);
  return 1;
}
