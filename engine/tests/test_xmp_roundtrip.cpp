// XMP 字节保留 round-trip 自测（对应 XMP-字段映射表 §9 验证基线，至少 5 个用例）
#include <cstdio>
#include <cstring>
#include <string>

#include <windows.h>  // Sleep（mtime 外部修改检测需要等待文件系统时间戳推进）

#include "core/md5.h"
#include "core/str_util.h"
#include "xmp/xmp_document.h"
#include "xmp/xmp_mapper.h"
#include "xmp/xmp_service.h"
#include "xmp/migrator.h"
#include "db/database.h"
#include "db/photo_dao.h"
#include "core/config.h"
#include "rpc/dispatcher.h"  // rpc::RpcError（checkExternal 的错误出口）

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

// 一份"存量 LR/LRTimelapse sidecar"样例：属性型 crs: + 元素型 crs: + lrt: + 未知命名空间
const char* kLegacySidecar =
    "<?xpacket begin=\"﻿\" id=\"W5M0MpCehiHzreSzNTczkc9d\"?>\n"
    "<x:xmpmeta xmlns:x=\"adobe:ns:meta/\">\n"
    " <rdf:RDF xmlns:rdf=\"http://www.w3.org/1999/02/22-rdf-syntax-ns#\">\n"
    "  <rdf:Description rdf:about=\"\" xmlns:crs=\"http://ns.adobe.com/camera-raw-settings/1.0/\" "
    "xmlns:lrt=\"http://lrtimelapse.com/ns/1.0/\" xmlns:vendor=\"http://example.com/vendor/1.0/\">\n"
    "   <crs:RawFileName>P1230001.RW2</crs:RawFileName>\n"
    "   <crs:Version>15.4</crs:Version>\n"
    "   <crs:ProcessVersion>15.4</crs:ProcessVersion>\n"
    "   <crs:WhiteBalance>Custom</crs:WhiteBalance>\n"
    "   <crs:Temperature>5600</crs:Temperature>\n"
    "   <crs:Exposure2012>+0.35</crs:Exposure2012>\n"
    "   <crs:Contrast2012>+22</crs:Contrast2012>\n"
    "   <crs:GradientBasedCorrections>\n"
    "    <rdf:Seq>\n"
    "     <rdf:li crs:CorrectionAmount=\"0.6\" crs:CorrectionMidpoint=\"45\" "
    "crs:CorrectionOpacity=\"100\" crs:CorrectionWhat=\"LinearGradient\"/>\n"
    "    </rdf:Seq>\n"
    "   </crs:GradientBasedCorrections>\n"
    "   <lrt:Keyframes>0,0,5600 50,0.35,5750 197,0,5500</lrt:Keyframes>\n"
    "   <vendor:Something>keep-me</vendor:Something>\n"
    "  </rdf:Description>\n"
    " </rdf:RDF>\n"
    "</x:xmpmeta>\n"
    "<?xpacket end=\"w\"?>\n";

}  // namespace

int main() {
  printf("=== XMP round-trip 自测 ===\n");

  // ---------- 用例 1：存量 sidecar 全字段解析无丢失 ----------
  {
    std::string err;
    std::unique_ptr<xmp::XmpDocument> doc = xmp::XmpDocument::parse(kLegacySidecar, &err);
    check(doc != nullptr, "case1: parse legacy sidecar");
    if (doc) {
      check(doc->get("crs", "WhiteBalance") == "Custom", "case1: crs:WhiteBalance=Custom");
      check(doc->get("crs", "Temperature") == "5600", "case1: crs:Temperature=5600");
      check(doc->getDouble("crs", "Exposure2012", 0) == 0.35, "case1: crs:Exposure2012=+0.35");
      check(doc->get("crs", "GradientBasedCorrections").find("CorrectionAmount=\"0.6\"") !=
                std::string::npos,
            "case1: crs:GradientBasedCorrections 结构完整");
      check(doc->get("lrt", "Keyframes") == "0,0,5600 50,0.35,5750 197,0,5500",
            "case1: lrt:Keyframes 解析");
      check(doc->get("vendor", "Something") == "keep-me", "case1: 未知命名空间字段解析");
      check(doc->has("crs", "Contrast2012"), "case1: crs:Contrast2012 存在");
    }
  }

  // ---------- 用例 2：round-trip 幂等（写→读→写，字节稳定） ----------
  {
    std::string err;
    std::unique_ptr<xmp::XmpDocument> a = xmp::XmpDocument::parse(kLegacySidecar, &err);
    a->setText("crs", "Exposure2012", "+1.25");
    a->setText("lumina", "SchemaVersion", "1");
    std::string first = a->serialize();
    std::unique_ptr<xmp::XmpDocument> b = xmp::XmpDocument::parse(first, &err);
    check(b != nullptr, "case2: re-parse after write");
    b->setText("crs", "Exposure2012", "+1.25");
    b->setText("lumina", "SchemaVersion", "1");
    std::string second = b->serialize();
    check(first == second, "case2: 写→读→写 字节稳定（幂等）");
    check(second.find("+1.25") != std::string::npos, "case2: 新值已写入");
  }

  // ---------- 用例 3：不污染 lrt: / vendor: 等他方字段 ----------
  {
    std::string err;
    std::unique_ptr<xmp::XmpDocument> doc = xmp::XmpDocument::parse(kLegacySidecar, &err);
    std::string lrtBefore = doc->get("lrt", "Keyframes");
    std::string vendorBefore = doc->get("vendor", "Something");
    std::string gradientBefore = doc->get("crs", "GradientBasedCorrections");
    std::string rawBefore = doc->get("crs", "RawFileName");
    std::string versionBefore = doc->get("crs", "Version");
    doc->setText("crs", "Exposure2012", "-0.80");
    doc->setText("crs", "Whites2012", "+12");
    doc->setText("lumina", "CullStatus", "pick");
    std::string out = doc->serialize();
    std::unique_ptr<xmp::XmpDocument> again = xmp::XmpDocument::parse(out, &err);
    check(again->get("lrt", "Keyframes") == lrtBefore, "case3: lrt:Keyframes 逐字节不变");
    check(again->get("vendor", "Something") == vendorBefore, "case3: vendor: 未知命名空间不变");
    check(again->get("crs", "GradientBasedCorrections") == gradientBefore,
          "case3: crs:GradientBasedCorrections 未触碰");
    check(again->get("crs", "RawFileName") == rawBefore, "case3: crs:RawFileName 未触碰");
    check(again->get("crs", "Version") == versionBefore, "case3: crs:Version 未触碰");
    check(again->get("crs", "Exposure2012") == "-0.80", "case3: 目标字段已更新");
    check(again->get("crs", "Whites2012") == "+12", "case3: 新字段已插入");
    check(again->get("lumina", "CullStatus") == "pick", "case3: lumina: 扩展可写");
  }

  // ---------- 用例 4：外部修改检测（mtime 机制） ----------
  {
    const std::string dir = "D:/LUMINA/build/runtime/test-xmp";
    makeDirs(dir);
    const std::string photoPath = dir + "/P9000001.dng";
    const std::string sidecarPath = dir + "/P9000001.xmp";
    removeFile(photoPath);
    removeFile(sidecarPath);
    // 构造最小 DB 环境
    core::Config config;
    std::string cfgErr;
    config.init(dir + "/home", &cfgErr);
    db::Database database;
    std::string dbErr;
    database.open(config.dbPath(), &dbErr);
    database.runMigrations(&dbErr);
    db::PhotoDao photos(&database);
    // photos.root_id / photos.folder_id 有外键约束（PRAGMA foreign_keys=ON），
    // 不建根直接 upsert 会 FOREIGN KEY constraint failed，photoId 保持 0 → 后续用例全线失真。
    const int64_t rootId = photos.ensureRoot(dir, "xmp-test", &dbErr);
    const int64_t folderId = photos.ensureFolder(rootId, ".", "xmp-test", &dbErr);
    PhotoRecord p;
    p.fileName = "P9000001.dng";
    p.filePath = photoPath;
    p.format = "RW2";
    p.rootId = rootId;
    p.folderId = folderId;
    p.sidecarPath = sidecarPath;
    p.capturedAt = nowMs();
    p.importedAt = nowMs();
    writeFileBytes(photoPath, "stub", &dbErr);
    int64_t photoId = 0;
    check(photos.upsert(p, &photoId, &dbErr) && photoId > 0, "case4: 建索引行");
    xmp::XmpService xmpService(&config, &photos, &database, nullptr);
    json::JsonValue params = xmp::XmpMapper::defaultParams();
    params.setItem("exposure", 0.5);
    std::string saveErr;
    check(xmpService.saveParams(photoId, params, &saveErr), "case4: 首次写入 sidecar");
    // 外部工具改写 sidecar（mtime 变化）
    std::string external = std::string(kLegacySidecar);
    external.replace(external.find("+0.35"), 5, "+9.99");
    Sleep(1200);  // 保证 mtime 变化可观测
    writeFileAtomic(sidecarPath, external, &saveErr);
    params.setItem("exposure", 1.0);
    bool blocked = !xmpService.saveParams(photoId, params, &saveErr);
    check(blocked && saveErr == "external conflict", "case4: 外部修改 → 拒绝覆盖（非静默）");
    rpc::RpcError checkErr;  // 不传 nullptr：出错分支会写 *err（传空指针会段错误）
    check(xmpService.checkExternal(photoId, &checkErr).get("changed").asBool(false) == true,
          "case4: checkExternal 报告 changed=true");
    database.close();
    removeFile(photoPath);
    removeFile(sidecarPath);
  }

  // ---------- 用例 5：迁移器 SchemaVersion 0/1 → 当前版本 ----------
  {
    // 构造一个 SchemaVersion=0 的 sidecar（无 lumina:SchemaVersion）
    std::string legacyV0 = kLegacySidecar;
    std::string err;
    std::unique_ptr<xmp::XmpDocument> doc = xmp::XmpDocument::parse(legacyV0, &err);
    check(!doc->has("lumina", "SchemaVersion"), "case5: 样本无 lumina:SchemaVersion");
    doc->setText("lumina", "SchemaVersion", "1");
    std::string migrated = doc->serialize();
    std::unique_ptr<xmp::XmpDocument> after = xmp::XmpDocument::parse(migrated, &err);
    check(after->getInt("lumina", "SchemaVersion", 0) == 1, "case5: 升级到 SchemaVersion=1");
    check(after->get("lrt", "Keyframes") == "0,0,5600 50,0.35,5750 197,0,5500",
          "case5: 迁移后 lrt: 数据不丢");
    check(after->get("vendor", "Something") == "keep-me", "case5: 迁移后未知命名空间不丢");
  }

  // ---------- 用例 6：新 sidecar 从零创建并回读 ----------
  {
    xmp::XmpDocument fresh = xmp::XmpDocument::createNew();
    fresh.setText("crs", "Exposure2012", "0.00");
    fresh.setText("lumina", "SchemaVersion", "1");
    fresh.setRaw("lumina", "Masks",
                 "[{\"id\":\"m1\",\"type\":\"linear\",\"enabled\":true,\"opacity\":90,"
                 "\"amount\":30,\"local\":{\"exposure\":0.2}}]");
    std::string out = fresh.serialize();
    std::string err;
    std::unique_ptr<xmp::XmpDocument> back = xmp::XmpDocument::parse(out, &err);
    check(back != nullptr, "case6: 新建 sidecar 可解析");
    if (back) {
      check(back->getDouble("crs", "Exposure2012", -1) == 0.0, "case6: crs:Exposure2012 回读");
      check(back->getInt("lumina", "SchemaVersion", 0) == 1, "case6: SchemaVersion 回读");
      check(back->get("lumina", "Masks").find("\"type\":\"linear\"") != std::string::npos,
            "case6: lumina:Masks JSON 回读");
      check(back->get("crs", "Exposure2012") == "0.00", "case6: 数值格式保留");
    }
  }

  // ---------- 用例 7：EditParams 映射 round-trip ----------
  {
    std::string err;
    std::unique_ptr<xmp::XmpDocument> doc = xmp::XmpDocument::parse(kLegacySidecar, &err);
    json::JsonValue params;
    std::vector<std::string> warnings;
    xmp::XmpMapper::readFromDoc(*doc, &params, &warnings);
    check(params.get("exposure").asDouble() == 0.35, "case7: exposure 映射 crs:Exposure2012");
    check(params.get("whiteBalance").asString() == "Custom",
          "case7: whiteBalance 映射 crs:WhiteBalance");
    check(params.get("temperature").asDouble() == 5600, "case7: temperature 映射 crs:Temperature");
    check(params.get("processVersion").asString() == "15.4", "case7: processVersion 读校验");
    xmp::XmpMapper::writeToDoc(params, doc.get());
    std::string out = doc->serialize();
    std::unique_ptr<xmp::XmpDocument> back = xmp::XmpDocument::parse(out, &err);
    json::JsonValue again;
    xmp::XmpMapper::readFromDoc(*back, &again, &warnings);
    check(again.get("exposure").asDouble() == params.get("exposure").asDouble(),
          "case7: 写回后 exposure 一致");
    check(back->get("lrt", "Keyframes") == "0,0,5600 50,0.35,5750 197,0,5500",
          "case7: lrt: 在参数写回后不变");
  }

  // ---------- 用例 8：HSL 8 色带 round-trip（WP-A3，hue/sat/lum × 8 = 24 字段） ----------
  {
    std::string err;
    std::unique_ptr<xmp::XmpDocument> doc = xmp::XmpDocument::parse(kLegacySidecar, &err);
    // UI 嵌套形状：hsl.{band}.{hue,sat,lum}（含 1.1.0 新增的 orange）
    auto band = [](double hue, double sat, double lum) {
      json::JsonValue b = json::JsonValue::objectValue();
      b.setItem("hue", hue);
      b.setItem("sat", sat);
      b.setItem("lum", lum);
      return b;
    };
    json::JsonValue hsl = json::JsonValue::objectValue();
    hsl.setItem("red", band(12.0, 0.0, 0.0));
    hsl.setItem("orange", band(0.0, -30.0, 0.0));
    hsl.setItem("blue", band(0.0, 0.0, 25.0));
    hsl.setItem("magenta", band(-5.0, 0.0, 0.0));
    json::JsonValue params = xmp::XmpMapper::defaultParams();
    params.setItem("hsl", hsl);
    xmp::XmpMapper::writeToDoc(params, doc.get());
    std::string out = doc->serialize();
    // sidecar 文本必须含 8 色带对应的 crs: 标准字段（含新增 Orange）
    check(out.find("crs:HueAdjustmentRed") != std::string::npos, "case8: crs:HueAdjustmentRed 落盘");
    check(out.find("crs:HueAdjustmentOrange") != std::string::npos,
          "case8: crs:HueAdjustmentOrange 落盘（1.1.0 新增）");
    check(out.find("crs:SaturationAdjustmentOrange") != std::string::npos,
          "case8: crs:SaturationAdjustmentOrange 落盘");
    check(out.find("crs:LuminanceAdjustmentBlue") != std::string::npos,
          "case8: crs:LuminanceAdjustmentBlue 落盘");
    check(out.find("crs:HueAdjustmentMagenta") != std::string::npos,
          "case8: crs:HueAdjustmentMagenta 落盘");
    // 读回形状必须与写入一致（嵌套 hsl）
    std::unique_ptr<xmp::XmpDocument> back = xmp::XmpDocument::parse(out, &err);
    json::JsonValue again;
    std::vector<std::string> warnings;
    xmp::XmpMapper::readFromDoc(*back, &again, &warnings);
    check(again.get("hsl").get("red").get("hue").asDouble() == 12.0,
          "case8: 读回 hsl.red.hue=12");
    check(again.get("hsl").get("orange").get("sat").asDouble() == -30.0,
          "case8: 读回 hsl.orange.sat=-30");
    check(again.get("hsl").get("blue").get("lum").asDouble() == 25.0,
          "case8: 读回 hsl.blue.lum=25");
    check(again.get("hsl").get("magenta").get("hue").asDouble() == -5.0,
          "case8: 读回 hsl.magenta.hue=-5");
    // 未写入的色带不得出现
    check(!back->has("crs", "HueAdjustmentGreen"), "case8: 未写入色带不产生字段");
    // 数值格式：formatNumber 整数不带小数点（LR 习惯）
    check(back->get("crs", "HueAdjustmentRed") == "12", "case8: 整数格式无小数点");
  }

  printf("=== 结果：%s（失败 %d 项） ===\n", gFailures == 0 ? "全部通过" : "存在失败", gFailures);
  return gFailures == 0 ? 0 : 1;
}
