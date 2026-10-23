// 导入执行：复制/校验/去重/入库/备份/预设应用，逐文件发 event.import.fileResult
#ifndef LUMINA_IMPORT_IMPORTER_H_
#define LUMINA_IMPORT_IMPORTER_H_

#include <functional>
#include <string>

#include "core/config.h"
#include "core/job_manager.h"
#include "db/album_dao.h"
#include "db/database.h"
#include "db/fts_indexer.h"
#include "db/photo_dao.h"
#include "import/scanner.h"
#include "json/json_value.h"
#include "raw/raw_decoder.h"
#include "xmp/xmp_service.h"

namespace lumina {
namespace import {

struct ImportOptions {
  std::string sourcePath;
  std::string targetPath;
  bool backupEnabled = false;
  std::string backupTarget;
  std::string backupTiming = "onImport";
  bool verifyMd5 = false;
  std::string renameTemplate = "{YYYY}-{MM}-{DD}_{camera}_{name}";
  bool pairRawJpg = true;
  std::string dedupe = "skip";
  bool enterCulling = false;
  std::string applyPreset;
};

class Importer {
 public:
  using NotifyFn = std::function<void(const std::string& method, const json::JsonValue& params)>;

  Importer(core::Config* config, db::PhotoDao* photos, db::Database* db, db::FtsIndexer* fts,
           raw::RawDecoder* decoder, xmp::XmpService* xmp, NotifyFn notify);

  // 在 job 线程内执行；进度与文件级结果通过 ctx / notify 上报
  void run(const ImportOptions& opts, JobContext& ctx);

 private:
  bool importOne(const ImportOptions& opts, const ScanItem& item, int64_t* photoId,
                 std::string* status, std::string* error);
  void probeMeta(const ScanItem& item, raw::RawMeta* meta);
  PhotoRecord buildRecord(const ImportOptions& opts, const ScanItem& item,
                          const std::string& finalPath, const raw::RawMeta& meta);

  core::Config* config_;
  db::PhotoDao* photos_;
  db::Database* db_;
  db::FtsIndexer* fts_;
  raw::RawDecoder* decoder_;
  xmp::XmpService* xmp_;
  NotifyFn notify_;
};

}  // namespace import
}  // namespace lumina

#endif  // LUMINA_IMPORT_IMPORTER_H_
