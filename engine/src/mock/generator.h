// 模拟图库生成器：程序化合成影像 → 未压缩 Linear DNG（TIFF-EP/CFA） + JPG + sidecar
// 说明：Panasonic RW2 为私有压缩容器，无法程序化构造；故生成"结构合法的未压缩 Linear DNG"，
//      让 LibRaw 真解码（L1 验证不伪造）。索引层元数据仍按真实基准标注 DC-S5M2 / RW2。
//      接真实图库时 LibRaw 直解 RW2，本模块代码无需改动。
#ifndef LUMINA_MOCK_GENERATOR_H_
#define LUMINA_MOCK_GENERATOR_H_

#include <string>

#include "core/config.h"
#include "core/job_manager.h"
#include "db/database.h"
#include "db/fts_indexer.h"
#include "db/health_dao.h"
#include "db/photo_dao.h"
#include "json/json_value.h"
#include "raw/raw_decoder.h"
#include "xmp/xmp_service.h"

namespace lumina {
namespace mock {

// 生成规格（默认值即真实基准分布，ipc-protocol §5.15）
struct MockSpec {
  int64_t total = 18192;
  int rootCount = 5;
  int folderCount = 79;
  std::string monthFrom = "2025-12";
  std::string monthTo = "2026-08";
  double rw2Ratio = 0.922;
  int64_t missing = 421;
  int64_t edited = 4347;
  double maskRatio = 0.838;
  int rating2 = 1365;
  int rating3 = 1;
  int rating4 = 57;
  int64_t gps = 137;
  int lrtGroups = 1;
  int imageWidth = 160;   // 生成 CFA 尺寸（控制磁盘占用；真实库为全尺寸）
  int imageHeight = 112;
  int pairCount = 600;    // RAW+JPG 配对数（一条目）
  int bracketGroups = 40; // 包围连拍组数（每组 3 张相邻照片，~2EV 阶梯，供 HDR 域端到端验证）
  bool embedPreview = true;
  // 留空则由 MockGenerator::run 从 settings.mockLibraryRoot 取（默认 {home}/mocklib）。
  // 禁止在此硬编码绝对路径：曾导致 DB 记录路径与 config 不一致，出现"图库在 A、配置说 B"。
  std::string libraryRoot;

  static bool fromJson(const json::JsonValue& params, MockSpec* out, std::string* err);
};

class MockGenerator {
 public:
  MockGenerator(core::Config* config, db::PhotoDao* photos, db::Database* db, db::FtsIndexer* fts,
                db::HealthDao* health, raw::RawDecoder* decoder, xmp::XmpService* xmp);

  void run(const MockSpec& spec, JobContext& ctx);
  json::JsonValue status(rpc::RpcError* err);

 private:
  // 在图库根写 README.md：说明模拟资产为结构合法的未压缩 Linear DNG（TIFF-EP/CFA），
  // 接真实图库时 LibRaw 直解 RW2 无需改代码（主理人红线要求写明）
  static void writeMockReadme(const std::string& libraryRoot);

  bool writeLinearDng(const std::string& path, int width, int height, int style, uint32_t seed,
                      const std::string& dateTime, int iso, double aperture, double shutter,
                      int focal);
  bool writeJpegAsset(const std::string& path, int width, int height, int style, uint32_t seed);

  core::Config* config_;
  db::PhotoDao* photos_;
  db::Database* db_;
  db::FtsIndexer* fts_;
  db::HealthDao* health_;
  raw::RawDecoder* decoder_;
  xmp::XmpService* xmp_;
  bool embedPreview_ = true;
};

}  // namespace mock
}  // namespace lumina

#endif  // LUMINA_MOCK_GENERATOR_H_
