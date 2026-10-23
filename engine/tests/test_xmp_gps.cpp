// XMP GPS 清除回归测试
// 复现路径：新建空 sidecar → setText 写 exif GPS → serialize → 重新 parse → remove → serialize
// 该路径下 remove 若失效，sidecar 里的定位将永远删不掉（map.clearPhotoLocation 失效）。
#include <cstdio>
#include <string>
#include <vector>

#include "xmp/xmp_document.h"

using lumina::xmp::XmpDocument;

static int g_fail = 0;

static void check(bool cond, const char* what) {
  if (!cond) {
    std::printf("  FAIL: %s\n", what);
    ++g_fail;
  } else {
    std::printf("  ok: %s\n", what);
  }
}

static bool hasKey(const std::vector<std::string>& keys, const char* k) {
  for (const auto& s : keys) {
    if (s == k) return true;
  }
  return false;
}

int main() {
  std::printf("=== case A: createNew -> set -> parse -> remove ===\n");
  {
    XmpDocument doc = XmpDocument::createNew();
    doc.setText("exif", "GPSLatitude", "31,5.976000N");
    doc.setText("exif", "GPSLongitude", "102,45.408000E");
    doc.setText("exif", "GPSAltitude", "3810.00");
    doc.setText("exif", "GPSAltitudeRef", "0");
    doc.setText("lumina", "LocationName", "四姑娘山·双桥沟口");
    std::string round1 = doc.serialize();
    std::printf("-- round1 (createNew 写入后) --\n%s\n", round1.c_str());
    check(round1.find("exif:GPSLatitude") != std::string::npos, "round1 含 GPSLatitude");

    std::string err;
    auto parsed = XmpDocument::parse(round1, &err);
    check(parsed != nullptr, "round1 可重新 parse");
    if (!parsed) return 1;

    auto keys = parsed->propertyKeys();
    std::printf("-- parsed keys (%zu) --\n", keys.size());
    for (const auto& s : keys) std::printf("   %s\n", s.c_str());
    check(hasKey(keys, "exif:GPSLatitude"), "parse 后能列出 exif:GPSLatitude");
    check(hasKey(keys, "exif:GPSLongitude"), "parse 后能列出 exif:GPSLongitude");

    check(parsed->has("exif", "GPSLatitude"), "has(exif,GPSLatitude) == true");

    parsed->remove("exif", "GPSLatitude");
    parsed->remove("exif", "GPSLongitude");
    parsed->remove("exif", "GPSAltitude");
    parsed->remove("exif", "GPSAltitudeRef");
    parsed->remove("lumina", "LocationName");
    check(!parsed->has("exif", "GPSLatitude"), "remove 后 has() == false");

    std::string round2 = parsed->serialize();
    std::printf("-- round2 (remove 后) --\n%s\n", round2.c_str());
    check(round2.find("exif:GPSLatitude") == std::string::npos, "round2 不含 GPSLatitude");
    check(round2.find("exif:GPSLongitude") == std::string::npos, "round2 不含 GPSLongitude");
    check(round2.find("lumina:LocationName") == std::string::npos, "round2 不含 LocationName");
    check(round2.find("<rdf:Description") != std::string::npos, "round2 仍有 rdf:Description");
    check(round2.find("</rdf:Description>") != std::string::npos, "round2 Description 闭合");
  }

  std::printf("\n=== case B: 已有 crs: 内容 + GPS（1848 路径） ===\n");
  {
    XmpDocument doc = XmpDocument::createNew();
    doc.setText("crs", "ProcessVersion", "15.4");
    doc.setText("crs", "Exposure2012", "0.600");
    doc.setText("exif", "GPSLatitude", "31,5.976000N");
    std::string round1 = doc.serialize();

    std::string err;
    auto parsed = XmpDocument::parse(round1, &err);
    if (!parsed) return 1;
    parsed->remove("exif", "GPSLatitude");
    std::string round2 = parsed->serialize();
    std::printf("-- round2 --\n%s\n", round2.c_str());
    check(round2.find("exif:GPSLatitude") == std::string::npos, "round2 不含 GPSLatitude");
    check(round2.find("crs:ProcessVersion") != std::string::npos, "crs:ProcessVersion 保留");
    check(round2.find("crs:Exposure2012") != std::string::npos, "crs:Exposure2012 保留");
  }

  std::printf("\n%s (failures=%d)\n", g_fail == 0 ? "ALL PASS" : "FAILED", g_fail);
  return g_fail == 0 ? 0 : 1;
}
