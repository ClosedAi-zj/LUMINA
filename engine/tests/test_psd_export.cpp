// M3-01c：PSD 导出集成测试
// 目的：用与 Exporter::exportOnePsd 完全相同的装配原语（plan::runEditPlan 渲染前缀 →
// xmp::XmpMapper::paramsToGraph 节点分组 → psd::writePsd + psd::readPsd），验证：
//   ① 分层文档结构正确（背景 + 各节点累积前缀 + 合成顶层）
//   ② flatten 一致性：合成顶层像素 == 合并图（PS 展平 == 顶层）
//   ③ keepLayers=false → 单层合成
//   ④ 节点数 > 20 → 自动降级为单层合成（不静默）
//   ⑤ 第三方独立解析（psd-indep-check.py 子进程）段边界闭合
// 注：本测试不调用 Exporter 私有方法（避免拉 DB/解码器桩），但复刻其装配算法，
//     与 exporter.cpp 的 exportOnePsd 保持同一口径；sRGB / 无水印下 applyColorSpace 为直通。
#include <cstdio>
#include <cstring>
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

int g_fail = 0;

void check(bool cond, const std::string& msg) {
  if (cond) {
    std::printf("  ✓ %s\n", msg.c_str());
  } else {
    std::printf("  ✗ %s\n", msg.c_str());
    ++g_fail;
  }
}

std::string fmtErr(const std::string& msg, int a, int b) {
  char buf[256];
  std::snprintf(buf, sizeof(buf), "%s（实际 %d / 预期 %d）", msg.c_str(), a, b);
  return std::string(buf);
}

// 生成 64x40 渐变图（便于肉眼/断言区分各层）
lumina::imaging::Image makeGradient(int w, int h) {
  lumina::imaging::Image img;
  img.width = w;
  img.height = h;
  img.pixels.assign(static_cast<size_t>(w) * h * 3, 0);
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      const size_t i = (static_cast<size_t>(y) * w + x) * 3;
      img.pixels[i] = static_cast<uint8_t>(x * 255 / (w - 1));
      img.pixels[i + 1] = static_cast<uint8_t>(y * 255 / (h - 1));
      img.pixels[i + 2] = static_cast<uint8_t>((x + y) * 255 / (w + h));
    }
  }
  return img;
}

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
    if (depth == 16) {
      b.push_back(v);
      b.push_back(v);
    } else {
      b.push_back(v);
    }
  }
  return b;
}

lumina::psd::Layer makeLayer(const std::string& name, const lumina::imaging::Image& img, int depth) {
  lumina::psd::Layer L;
  L.name = name;
  L.top = 0;
  L.left = 0;
  L.width = img.width;
  L.height = img.height;
  L.opacity = 255;
  L.blendKey = "norm";
  L.visible = true;
  L.rgb = toBytes(img, depth);
  return L;
}

// 复刻 exportOnePsd 装配算法（sRGB 直通、无水印）
bool assemble(const lumina::imaging::Image& base, const lumina::json::JsonValue& editParams,
              bool haveParams, bool keepLayers, int depth,
              lumina::psd::Document* doc, std::string* warning) {
  lumina::imaging::Image composite = base;
  if (haveParams) {
    std::string rerr;
    composite = renderParams(editParams, base, &rerr);
    if (!rerr.empty()) return false;
  }
  doc->composite = toBytes(composite, depth);

  std::vector<lumina::psd::Layer> layers;
  layers.push_back(makeLayer("背景 · 原片", base, depth));

  bool layered = false;
  if (keepLayers && haveParams) {
    lumina::graph::NodeGraph g = lumina::xmp::XmpMapper::paramsToGraph(editParams);
    if (static_cast<int>(g.nodes.size()) > 20) {
      if (warning) *warning = "节点数超 20，降级单层";
    } else {
      lumina::json::JsonValue prefix = lumina::json::JsonValue::objectValue();
      bool okAll = true;
      for (const lumina::graph::GraphNode& node : g.nodes) {
        if (node.params.isObject()) {
          for (const auto& kv : node.params.objectItems()) prefix.setItem(kv.first, kv.second);
        }
        std::string rerr;
        lumina::imaging::Image rendered = renderParams(prefix, base, &rerr);
        if (!rerr.empty()) {
          if (warning) *warning = "前缀渲染失败: " + rerr;
          okAll = false;
          break;
        }
        lumina::psd::Layer L = makeLayer("N-" + node.id, rendered, depth);
        L.luminaSnapshot = node.params.serialize();
        layers.push_back(L);
      }
      layered = okAll;
    }
  }
  layers.push_back(makeLayer("合成 · 最终", composite, depth));
  if (layered) {
    doc->layers = std::move(layers);
  } else {
    doc->layers.clear();
    doc->layers.push_back(makeLayer("合成 · 最终", composite, depth));
  }
  return true;
}

void runCase(const char* tag, const lumina::json::JsonValue& params, bool keepLayers, int depth,
             int expectedLayerCount, bool expectLayered) {
  std::printf("用例 %s (keepLayers=%d depth=%d):\n", tag, keepLayers ? 1 : 0, depth);
  lumina::imaging::Image base = makeGradient(64, 40);
  bool have = params.isObject() && !params.objectItems().empty();
  lumina::psd::Document doc;
  std::string warn;
  if (!assemble(base, params, have, keepLayers, depth, &doc, &warn)) {
    check(false, "装配失败");
    return;
  }
  const std::string path = std::string("m3-psd-export-") + tag + ".psd";
  lumina::psd::WriteOptions opts;
  opts.width = base.width;
  opts.height = base.height;
  opts.depth = depth;
  opts.rle = true;
  opts.writeComposite = true;
  opts.dpi = 72;
  std::string werr;
  check(lumina::psd::writePsd(path, doc, opts, &werr), ("写出 " + path + (werr.empty() ? "" : " : " + werr)).c_str());

  lumina::psd::PsdInfo info;
  std::string rerr;
  check(lumina::psd::readPsd(path, &info, &rerr), ("读回 " + path + (rerr.empty() ? "" : " : " + rerr)).c_str());
  if (rerr.empty()) {
    const int n = static_cast<int>(info.layers.size());
    check(n == expectedLayerCount,
          fmtErr("层数与预期一致", n, expectedLayerCount));
    // flatten 一致性：顶层 == 合并图
    const lumina::psd::ReadLayer& top = info.layers.back();
    check(top.rgb == info.composite, "顶层像素 == 合并图（flatten 一致）");
    check(info.composite.size() == static_cast<size_t>(base.width) * base.height * 3 * (depth / 8),
          "合并图字节长度正确");
    // 每层 rgb 长度正确
    bool allLen = true;
    for (const auto& L : info.layers) {
      if (L.rgb.size() != static_cast<size_t>(L.width) * L.height * 3 * (depth / 8)) allLen = false;
    }
    check(allLen, "每层 RGB 字节长度正确");
    if (expectLayered) {
      check(info.hasLuminaSnapshot, "分层文档含 lumina 节点快照块");
    }
  }

  // 第三方独立解析（子进程）：段边界闭合
  std::string cmd = "python ../../build/psd-indep-check.py " + path;
  int rc = std::system(cmd.c_str());
  check(rc == 0, "第三方独立解析通过（段边界闭合）");
}

}  // namespace

int main() {
  // 多节点参数（basic + curve + color + calibration，触发 4 个非空节点）
  lumina::json::JsonValue params = lumina::json::JsonValue::objectValue();
  params.setItem("exposure", lumina::json::JsonValue::number(0.5));
  params.setItem("contrast", lumina::json::JsonValue::number(12.0));
  params.setItem("toneCurveRGB",
                 lumina::json::JsonValue::str("0,0 90,70 180,210 255,255"));
  lumina::json::JsonValue hsl = lumina::json::JsonValue::objectValue();
  lumina::json::JsonValue red = lumina::json::JsonValue::objectValue();
  red.setItem("hue", lumina::json::JsonValue::number(10.0));
  red.setItem("sat", lumina::json::JsonValue::number(20.0));
  hsl.setItem("red", red);
  params.setItem("hsl", hsl);
  params.setItem("calibration", []() {
    lumina::json::JsonValue c = lumina::json::JsonValue::objectValue();
    c.setItem("red", lumina::json::JsonValue::number(5.0));
    return c;
  }());

  // 8 全局节点 + 4 非空 → 分层时 bg(1) + 8 + 合成(1) = 10 层
  runCase("layered8", params, true, 8, 10, true);

  // keepLayers=false → 单层（仅合成）
  runCase("single", params, false, 8, 1, false);

  // 16bit 分层
  runCase("layered16", params, true, 16, 10, true);

  // >20 节点（adjustmentStack 21 条）→ 降级单层
  lumina::json::JsonValue many = lumina::json::JsonValue::objectValue();
  lumina::json::JsonValue stack = lumina::json::JsonValue::arrayValue();
  for (int i = 0; i < 21; ++i) {
    lumina::json::JsonValue layer = lumina::json::JsonValue::objectValue();
    layer.setItem("type", lumina::json::JsonValue::str("Basic"));
    layer.setItem("opacity", lumina::json::JsonValue::number(100.0));
    layer.setItem("blendMode", lumina::json::JsonValue::str("normal"));
    lumina::json::JsonValue lp = lumina::json::JsonValue::objectValue();
    lp.setItem("exposure", lumina::json::JsonValue::number(0.1));
    layer.setItem("params", lp);
    stack.addItem(layer);
  }
  many.setItem("adjustmentStack", stack);
  runCase("over20", many, true, 8, 1, false);

  if (g_fail == 0) {
    std::printf("test_psd_export: ALL PASS\n");
    return 0;
  }
  std::printf("test_psd_export: %d FAILED\n", g_fail);
  return 1;
}
