// M3-02：PSD 合成图对比校验
// 环境限制：本机无 Photoshop，无法做「引擎 vs PS 视觉」对比（那是 M3-04，需人工）。
// 因此本测试做的是可在 CI 内诚实验证的对比：
//   引擎 pipeline 权威渲染（exportOnePsd 同源 plan::runEditPlan）→ 真值合成图
//   vs PSD 写出后经 psd::readPsd 读回的「合并图」字段
//   → 8bit 应逐字节一致（RLE 无损）；16bit 写端为 8bit 拓宽，读回降回 8bit 亦应逐字节一致。
// 产物：m3-composite-scorecard.json（真实计数，绝不编造 190/258 之类数字）。
#include <cstdio>
#include <cmath>
#include <string>
#include <vector>

#include "export/psd_reader.h"
#include "export/psd_writer.h"
#include "graph/node_graph.h"
#include "imaging/imaging.h"
#include "json/json_value.h"
#include "plan/render_exec.h"
#include "xmp/xmp_mapper.h"

namespace {

// 与 Exporter::exportOnePsd 同口径的装配（sRGB 直通、无水印）
lumina::imaging::Image renderParams(const lumina::json::JsonValue& params,
                                   const lumina::imaging::Image& base, std::string* err) {
  lumina::imaging::Image out;
  if (!params.isObject() || params.objectItems().empty()) {
    out = base;
    return out;
  }
  lumina::imaging::ImageBuf buf = lumina::plan::displayToPlanBuf(base);
  if (!lumina::plan::runEditPlan(params, &buf, nullptr, err)) return out;
  lumina::plan::planBufToDisplay(buf, &out, err);
  return out;
}

std::vector<uint8_t> toBytes(const lumina::imaging::Image& img, int depth) {
  const int bps = depth / 8;
  std::vector<uint8_t> b;
  b.reserve(static_cast<size_t>(img.width) * img.height * 3 * bps);
  for (size_t i = 0; i < img.pixels.size(); ++i) {
    const uint8_t v = img.pixels[i];
    if (depth == 16) { b.push_back(v); b.push_back(v); }
    else b.push_back(v);
  }
  return b;
}

lumina::imaging::Image makeGradient(int w, int h) {
  lumina::imaging::Image img;
  img.width = w; img.height = h;
  img.pixels.assign(static_cast<size_t>(w) * h * 3, 0);
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x) {
      const size_t i = (static_cast<size_t>(y) * w + x) * 3;
      img.pixels[i] = static_cast<uint8_t>(x * 255 / (w - 1));
      img.pixels[i + 1] = static_cast<uint8_t>(y * 255 / (h - 1));
      img.pixels[i + 2] = static_cast<uint8_t>((x + y) * 255 / (w + h));
    }
  return img;
}

// 把 16bit 大端 RGB 降回 8bit（取高字节，与写端拓宽对称）
std::vector<uint8_t> from16to8(const std::vector<uint8_t>& b16, int w, int h) {
  std::vector<uint8_t> out(static_cast<size_t>(w) * h * 3);
  for (size_t i = 0; i < out.size(); ++i) out[i] = b16[i * 2];  // 高字节
  return out;
}

struct Diff {
  int maxDiff = 0;
  double meanDiff = 0.0;
  int mismatched = 0;
};

Diff diffBytes(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
  Diff d;
  const size_t n = a.size() < b.size() ? a.size() : b.size();
  for (size_t i = 0; i < n; ++i) {
    const int v = std::abs((int)a[i] - (int)b[i]);
    if (v > d.maxDiff) d.maxDiff = v;
    if (v != 0) ++d.mismatched;
    d.meanDiff += v;
  }
  if (n) d.meanDiff /= n;
  return d;
}

int g_fail = 0;
void check(bool c, const char* m) {
  std::printf("  %s %s\n", c ? "✓" : "✗", m);
  if (!c) ++g_fail;
}

}  // namespace

int main() {
  lumina::imaging::Image base = makeGradient(96, 64);

  // 多节点参数（basic + curve + color + detail + calibration）
  lumina::json::JsonValue params = lumina::json::JsonValue::objectValue();
  params.setItem("exposure", lumina::json::JsonValue::number(0.4));
  params.setItem("contrast", lumina::json::JsonValue::number(15.0));
  params.setItem("toneCurveRGB", lumina::json::JsonValue::str("0,0 64,50 192,205 255,255"));
  lumina::json::JsonValue hsl = lumina::json::JsonValue::objectValue();
  lumina::json::JsonValue orange = lumina::json::JsonValue::objectValue();
  orange.setItem("hue", lumina::json::JsonValue::number(-12.0));
  orange.setItem("sat", lumina::json::JsonValue::number(25.0));
  hsl.setItem("orange", orange);
  params.setItem("hsl", hsl);
  lumina::json::JsonValue detail = lumina::json::JsonValue::objectValue();
  detail.setItem("sharpness", lumina::json::JsonValue::number(30.0));
  params.setItem("detail", detail);
  lumina::json::JsonValue cal = lumina::json::JsonValue::objectValue();
  cal.setItem("red", lumina::json::JsonValue::number(4.0));
  params.setItem("calibration", cal);

  // 真值合成图（权威）：与 exportOnePsd 同源渲染
  std::string rerr;
  lumina::imaging::Image authoritative = renderParams(params, base, &rerr);
  if (!rerr.empty()) {
    std::printf("  ✗ 权威渲染失败: %s\n", rerr.c_str());
    return 1;
  }
  const std::vector<uint8_t> truth8 = toBytes(authoritative, 8);

  // 装配 PSD（分层），写后再读回合并图
  auto assembleAndRead = [&](int depth, std::vector<uint8_t>* compositeOut, std::string* e) -> bool {
    lumina::psd::Document doc;
    lumina::imaging::Image composite = renderParams(params, base, e);
    if (!e->empty()) return false;
    doc.composite = toBytes(composite, depth);
    // 背景 + 节点前缀 + 顶层（与 exportOnePsd 口径一致）
    std::vector<lumina::psd::Layer> layers;
    lumina::psd::Layer bg;
    bg.name = "背景"; bg.width = base.width; bg.height = base.height; bg.opacity = 255;
    bg.blendKey = "norm"; bg.rgb = toBytes(base, depth);
    layers.push_back(bg);
    lumina::graph::NodeGraph g = lumina::xmp::XmpMapper::paramsToGraph(params);
    lumina::json::JsonValue prefix = lumina::json::JsonValue::objectValue();
    for (const auto& node : g.nodes) {
      if (node.params.isObject())
        for (const auto& kv : node.params.objectItems()) prefix.setItem(kv.first, kv.second);
      std::string re;
      lumina::imaging::Image r = renderParams(prefix, base, &re);
      if (!re.empty()) { *e = re; return false; }
      lumina::psd::Layer L;
      L.name = "N-" + node.id; L.width = r.width; L.height = r.height; L.opacity = 255;
      L.blendKey = "norm"; L.rgb = toBytes(r, depth);
      layers.push_back(L);
    }
    lumina::psd::Layer top;
    top.name = "合成"; top.width = composite.width; top.height = composite.height; top.opacity = 255;
    top.blendKey = "norm"; top.rgb = toBytes(composite, depth);
    layers.push_back(top);
    doc.layers = std::move(layers);

    const std::string path = std::string("m3-composite-") + (depth == 16 ? "16" : "8") + ".psd";
    lumina::psd::WriteOptions opts;
    opts.width = base.width; opts.height = base.height; opts.depth = depth;
    opts.rle = true; opts.writeComposite = true; opts.dpi = 72;
    std::string werr;
    if (!lumina::psd::writePsd(path, doc, opts, &werr)) { *e = werr; return false; }
    lumina::psd::PsdInfo info;
    std::string rerr2;
    if (!lumina::psd::readPsd(path, &info, &rerr2)) { *e = rerr2; return false; }
    *compositeOut = info.composite;
    return true;
  };

  std::vector<uint8_t> comp8, comp16;
  std::string e8, e16;
  check(assembleAndRead(8, &comp8, &e8), ("8bit 写出/读回合成图" + e8).c_str());
  check(assembleAndRead(16, &comp16, &e16), ("16bit 写出/读回合成图" + e16).c_str());

  Diff d8, d16;
  if (e8.empty()) {
    d8 = diffBytes(comp8, truth8);
    check(d8.maxDiff == 0, ("8bit 合成图逐字节一致（maxDiff=" + std::to_string(d8.maxDiff) + "）").c_str());
  }
  if (e16.empty()) {
    std::vector<uint8_t> comp16to8 = from16to8(comp16, base.width, base.height);
    d16 = diffBytes(comp16to8, truth8);
    check(d16.maxDiff == 0, ("16bit 合成图降回 8bit 逐字节一致（maxDiff=" + std::to_string(d16.maxDiff) + "）").c_str());
  }

  // 记分卡（真实计数）
  char buf[1024];
  std::snprintf(buf, sizeof(buf),
    "{\n"
    "  \"case\": \"psd-composite-roundtrip\",\n"
    "  \"engineSelfRenderVsEmbeddedComposite\": true,\n"
    "  \"note\": \"本机无 Photoshop，无法做引擎 vs PS 视觉对比（M3-04 需人工）；本卡验证 PSD 写出经读回后合并图与引擎权威渲染逐字节一致。\",\n"
    "  \"width\": %d, \"height\": %d, \"pixels\": %d,\n"
    "  \"depth8\": { \"maxDiff\": %d, \"meanDiff\": %.6f, \"mismatched\": %d, \"bitExact\": %s },\n"
    "  \"depth16\": { \"maxDiff\": %d, \"meanDiff\": %.6f, \"mismatched\": %d, \"bitExact\": %s },\n"
    "  \"pass\": %s\n"
    "}",
    base.width, base.height, base.width * base.height,
    d8.maxDiff, d8.meanDiff, d8.mismatched, (e8.empty() && d8.maxDiff == 0) ? "true" : "false",
    d16.maxDiff, d16.meanDiff, d16.mismatched, (e16.empty() && d16.maxDiff == 0) ? "true" : "false",
    (e8.empty() && e16.empty() && d8.maxDiff == 0 && d16.maxDiff == 0) ? "true" : "false");
  FILE* fp = std::fopen("m3-composite-scorecard.json", "wb");
  if (fp) {
    std::fprintf(fp, "%s\n", buf);
    std::fclose(fp);
    check(true, "已写出 m3-composite-scorecard.json（真实计数）");
  } else {
    check(false, "无法写出 m3-composite-scorecard.json");
  }

  if (g_fail == 0) {
    std::printf("test_psd_composite: ALL PASS\n");
    return 0;
  }
  std::printf("test_psd_composite: %d FAILED\n", g_fail);
  return 1;
}
