// ToneCurve 文本编解码测试（v1.2.0 P0 回归：parseCurve 曾把 "0, 0 255, 255" 读成 y 全零，
// 含曲线照片预览全黑）。覆盖：LR 三种格式、多点、截断对、空串回退、往返、全零拒收、
// 以及 XmpMapper::readFromDoc 全链路（LR 风格 sidecar → toneCurveRGB）。
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "xmp/xmp_document.h"
#include "xmp/xmp_mapper.h"

using namespace lumina;

namespace {

int gFailures = 0;

void check(bool condition, const std::string& name) {
  if (condition) {
    printf("  [PASS] %s\n", name.c_str());
  } else {
    printf("  [FAIL] %s\n", name.c_str());
    ++gFailures;
  }
}

struct Pt {
  double x;
  double y;
};

std::vector<Pt> toPoints(const json::JsonValue& curve) {
  std::vector<Pt> pts;
  if (!curve.isArray()) return pts;
  for (const auto& p : curve.arrayItems()) pts.push_back({p.get("x").asDouble(), p.get("y").asDouble()});
  return pts;
}

bool eq(double a, double b) { return (a - b) < 1e-9 && (b - a) < 1e-9; }

bool isIdentity(const std::vector<Pt>& pts) {
  return pts.size() == 2 && eq(pts[0].x, 0) && eq(pts[0].y, 0) && eq(pts[1].x, 255) &&
         eq(pts[1].y, 255);
}

json::JsonValue makeCurve(const std::vector<Pt>& pts) {
  json::JsonValue arr = json::JsonValue::arrayValue();
  for (const Pt& p : pts) {
    json::JsonValue o = json::JsonValue::objectValue();
    o.setItem("x", p.x);
    o.setItem("y", p.y);
    arr.addItem(o);
  }
  return arr;
}

// 一份"Lightroom 写出的 sidecar"风格样例（属性型 crs: 字段 + 元素型曲线文本）
const char* kLrSidecar =
    "<?xpacket begin=\"﻿\" id=\"W5M0MpCehiHzreSzNTczkc9d\"?>\n"
    "<x:xmpmeta xmlns:x=\"adobe:ns:meta/\">\n"
    " <rdf:RDF xmlns:rdf=\"http://www.w3.org/1999/02/22-rdf-syntax-ns#\">\n"
    "  <rdf:Description rdf:about=\"\" xmlns:crs=\"http://ns.adobe.com/camera-raw-settings/1.0/\">\n"
    "   <crs:Version>15.4</crs:Version>\n"
    "   <crs:ProcessVersion>15.4</crs:ProcessVersion>\n"
    "   <crs:ToneCurvePV2012>0, 0 255, 255</crs:ToneCurvePV2012>\n"
    "   <crs:ToneCurvePV2012Red>0, 0 64, 70 255, 255</crs:ToneCurvePV2012Red>\n"
    "  </rdf:Description>\n"
    " </rdf:RDF>\n"
    "</x:xmpmeta>\n"
    "<?xpacket end=\"w\"?>\n";

}  // namespace

int main(int argc, char** argv) {
  printf("== test_xmp_curve ==\n");

  // 用例 1：LR 标准格式 "0, 0 255, 255"（逗号+空格）
  {
    std::vector<Pt> pts = toPoints(xmp::parseCurve("0, 0 255, 255"));
    check(pts.size() == 2, "case1: 两点");
    check(pts.size() == 2 && eq(pts[0].y, 0) && eq(pts[1].y, 255),
          "case1: y=0 与 y=255（全零拒收断言：修前旧算法 y 全零）");
    check(pts.size() == 2 && eq(pts[1].y, 255) && pts[1].y != 0.0, "case1: y 非零");
  }

  // 用例 2：无空格紧凑格式 "0,0 128,128 255,255"
  {
    std::vector<Pt> pts = toPoints(xmp::parseCurve("0,0 128,128 255,255"));
    check(pts.size() == 3, "case2: 三点（紧凑格式）");
    check(pts.size() == 3 && eq(pts[1].x, 128) && eq(pts[1].y, 128), "case2: 中点 128,128");
  }

  // 用例 3：四点且 y 单调递增
  {
    std::vector<Pt> pts = toPoints(xmp::parseCurve("0, 0 64, 80 128, 140 255, 255"));
    check(pts.size() == 4, "case3: 四点");
    bool mono = pts.size() == 4 && pts[0].y < pts[1].y && pts[1].y < pts[2].y &&
                pts[2].y < pts[3].y;
    check(mono, "case3: y 单调递增 0<80<140<255");
    check(pts.size() == 4 && eq(pts[2].x, 128) && eq(pts[2].y, 140), "case3: 点 (128,140)");
  }

  // 用例 4：末尾不完整对，不崩溃。单点无法用于渲染，按规则回退恒等曲线
  // （不完整对被跳过后只剩 1 点 < 2 → identity fallback，绝不输出全零/空数组）
  {
    std::vector<Pt> a = toPoints(xmp::parseCurve("0, 0 255"));
    check(isIdentity(a), "case4a: '0, 0 255' 截断 → 恒等回退不崩");
    std::vector<Pt> b = toPoints(xmp::parseCurve("0, 0 255,"));
    check(isIdentity(b), "case4b: '0, 0 255,' 孤立 x, → 恒等回退不崩");
  }

  // 用例 5：空串 / 乱码 → 恒等两点 + warning（warning 走日志，不参与断言）
  {
    check(isIdentity(toPoints(xmp::parseCurve(""))), "case5a: 空串 → 恒等两点");
    check(isIdentity(toPoints(xmp::parseCurve("abc def ,,"))), "case5b: 乱码 → 恒等两点");
  }

  // 用例 6：往返（写 → 读 → 逐点相等），含恒等与自定义曲线
  {
    std::vector<Pt> orig = {{0, 0}, {64, 80}, {128, 140}, {255, 255}};
    std::string text = xmp::serializeCurve(makeCurve(orig));
    std::vector<Pt> back = toPoints(xmp::parseCurve(text));
    bool same = back.size() == orig.size();
    for (size_t i = 0; same && i < orig.size(); ++i)
      same = eq(back[i].x, orig[i].x) && eq(back[i].y, orig[i].y);
    check(same, "case6a: 4 点曲线往返逐点相等");
    std::vector<Pt> ident = {{0, 0}, {255, 255}};
    std::vector<Pt> back2 = toPoints(xmp::parseCurve(xmp::serializeCurve(makeCurve(ident))));
    check(isIdentity(back2), "case6b: 恒等曲线往返一致");
  }

  // 用例 7：换行分隔（LR 导出变体）
  {
    std::vector<Pt> pts = toPoints(xmp::parseCurve("0, 0\n128, 60\n255, 255"));
    check(pts.size() == 3 && eq(pts[1].x, 128) && eq(pts[1].y, 60), "case7: 换行分隔三点");
  }

  // 用例 8：全链路 —— LR 风格 sidecar → XmpMapper::readFromDoc → toneCurveRGB 非零
  {
    std::string parseErr;
    std::unique_ptr<xmp::XmpDocument> doc = xmp::XmpDocument::parse(kLrSidecar, &parseErr);
    check(doc != nullptr, "case8: LR sidecar 解析成功");
    if (doc) {
      json::JsonValue params = json::JsonValue::objectValue();
      std::vector<std::string> warnings;
      xmp::XmpMapper::readFromDoc(*doc, &params, &warnings);
      std::vector<Pt> rgb = toPoints(params.get("toneCurveRGB"));
      std::vector<Pt> red = toPoints(params.get("toneCurveR"));
      check(rgb.size() == 2 && eq(rgb[1].y, 255), "case8: toneCurveRGB 读回 y=255 非零");
      check(red.size() == 3 && eq(red[1].y, 70), "case8: toneCurveR 三点读回 (64,70)");
      bool allZero = true;
      for (const Pt& p : rgb) if (p.y != 0.0) allZero = false;
      check(!allZero, "case8: 反退化断言 —— 曲线 y 不得全零（修前旧算法即全零）");
    }
  }

  // 用例 9（可选探针）：argv[1] 传入真实 sidecar 文本文件（只读），验证全链路非零
  if (argc > 1) {
    FILE* f = fopen(argv[1], "rb");
    if (!f) {
      printf("  [FAIL] case9: 无法打开 %s\n", argv[1]);
      ++gFailures;
    } else {
      std::string bytes;
      char buf[4096];
      size_t n;
      while ((n = fread(buf, 1, sizeof(buf), f)) > 0) bytes.append(buf, n);
      fclose(f);
      std::string parseErr;
      std::unique_ptr<xmp::XmpDocument> doc = xmp::XmpDocument::parse(bytes, &parseErr);
      if (doc) {
        json::JsonValue params = json::JsonValue::objectValue();
        std::vector<std::string> warnings;
        xmp::XmpMapper::readFromDoc(*doc, &params, &warnings);
        std::vector<Pt> rgb = toPoints(params.get("toneCurveRGB"));
        bool nonZero = false;
        for (const Pt& p : rgb) if (p.y != 0.0) nonZero = true;
        printf("  [INFO] case9: %s toneCurveRGB points=%zu nonZero=%s\n", argv[1],
               rgb.size(), nonZero ? "true" : "false");
        check(nonZero, "case9: 真实 sidecar 曲线 y 非零（只读探针）");
      } else {
        printf("  [INFO] case9: sidecar 解析失败 %s（跳过）\n", parseErr.c_str());
      }
    }
  }

  printf("== test_xmp_curve %s ==\n", gFailures == 0 ? "ALL PASS" : "FAILED");
  return gFailures == 0 ? 0 : 1;
}
