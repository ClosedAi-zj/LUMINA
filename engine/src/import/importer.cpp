// 导入执行实现
#include "import/importer.h"

#include <set>

#include "core/logger.h"
#include "core/md5.h"
#include "core/str_util.h"
#include "import/rename.h"
#include "metadata/exif_reader.h"
#include "metadata/video_probe.h"
#include "raw/embedded_proxy.h"

namespace lumina {
namespace import {
namespace {

// f/2.8 显示
std::string formatNumberAperture(double f) {
  char buf[16];
  snprintf(buf, sizeof(buf), "f/%.1f", f);
  return std::string(buf);
}

// 1/125s 显示（小于 1s 用分数形式）
std::string formatNumberShutter(double seconds) {
  if (seconds <= 0.0) return std::string();
  char buf[24];
  if (seconds >= 1.0) {
    snprintf(buf, sizeof(buf), "%.1fs", seconds);
  } else {
    snprintf(buf, sizeof(buf), "1/%ds", static_cast<int>(1.0 / seconds + 0.5));
  }
  return std::string(buf);
}

}  // namespace

Importer::Importer(core::Config* config, db::PhotoDao* photos, db::Database* db,
                   db::FtsIndexer* fts, raw::RawDecoder* decoder, xmp::XmpService* xmp,
                   NotifyFn notify)
    : config_(config), photos_(photos), db_(db), fts_(fts), decoder_(decoder), xmp_(xmp),
      notify_(std::move(notify)) {}

// 元数据探测（RAW 走 LibRaw，JPEG/PNG 走 EXIF，视频走容器探测）。
// importOne 在重命名之前就要用（拍摄时间/机型进文件名），故与 buildRecord 分离、
// 整个导入只探测一次，不重复打开文件。
void Importer::probeMeta(const ScanItem& item, raw::RawMeta* meta) {
  if (item.isRaw) {
    std::string err;
    decoder_->readMetadata(item.path, meta, &err);
  } else if (item.isJpg) {
    metadata::ExifInfo exif;
    std::string err;
    if (metadata::readExifFromFile(item.path, &exif, &err)) {
      meta->make = exif.make;
      meta->model = exif.model;
      meta->lens = exif.lens;
      meta->iso = exif.iso;
      meta->aperture = exif.aperture;
      meta->shutter = exif.shutter;
      meta->focalLength = exif.focalLength;
      meta->timestampMs = exif.timestampMs;
      meta->width = exif.width;
      meta->height = exif.height;
      meta->hasGps = exif.hasGps;
      meta->gpsLat = exif.gpsLat;
      meta->gpsLng = exif.gpsLng;
      meta->gpsAlt = exif.gpsAlt;
    }
  } else if (item.isVideo) {
    metadata::VideoInfo video;
    std::string err;
    if (metadata::probeVideo(item.path, &video, &err)) {
      meta->width = video.width;
      meta->height = video.height;
      meta->timestampMs = video.creationMs;
    }
  }
}

PhotoRecord Importer::buildRecord(const ImportOptions& opts, const ScanItem& item,
                                  const std::string& finalPath, const raw::RawMeta& meta) {
  PhotoRecord p;
  p.filePath = finalPath;
  p.fileName = baseName(finalPath);
  p.format = item.format;
  p.fileSize = fileSize(finalPath);
  p.importedAt = nowMs();
  p.health = "unchecked";
  p.isVideo = item.isVideo;
  if (meta.width > 0 && meta.height > 0) {
    p.width = meta.width;
    p.height = meta.height;
  }
  p.capturedAt = meta.timestampMs > 0 ? meta.timestampMs : item.mtimeMs;
  p.camera = meta.model.empty() ? meta.make : meta.model;
  p.lens = meta.lens;
  p.iso = meta.iso;
  p.aperture = meta.aperture > 0 ? formatNumberAperture(meta.aperture) : std::string();
  p.shutter = meta.shutter > 0 ? formatNumberShutter(meta.shutter) : std::string();
  p.focalLength = meta.focalLength > 0 ? std::to_string(meta.focalLength) + "mm" : std::string();
  p.hasGps = meta.hasGps;
  p.gpsLat = meta.gpsLat;
  p.gpsLng = meta.gpsLng;
  p.gpsAlt = meta.gpsAlt;
  p.sidecarPath = replaceExtension(finalPath, "xmp");
  return p;
}

bool Importer::importOne(const ImportOptions& opts, const ScanItem& item, int64_t* photoId,
                         std::string* status, std::string* error) {
  *status = "ok";
  // 目标相对结构
  std::string relative = item.path.substr(opts.sourcePath.size());
  while (!relative.empty() && (relative[0] == '/' || relative[0] == '\\')) relative.erase(0, 1);
  std::string relativeDir = parentPath(relative);

  // 元数据先探测（重命名要用真实拍摄时间与机型；缺 EXIF 日期才回退 mtime）
  raw::RawMeta meta;
  probeMeta(item, &meta);
  const int64_t capturedAtMs = meta.timestampMs > 0 ? meta.timestampMs : item.mtimeMs;

  // 重命名
  RenameContext ctx;
  ctx.capturedAtMs = capturedAtMs;
  ctx.camera = meta.model.empty() ? meta.make : meta.model;
  ctx.originalName = baseNameNoExt(item.path);
  ctx.extension = extensionOf(item.path);
  std::string newBase = RenameEngine::apply(opts.renameTemplate, ctx);
  std::string targetDir = joinPath(opts.targetPath, relativeDir);
  std::string finalPath = joinPath(targetDir, newBase + "." + ctx.extension);

  // 去重：同路径已在索引且文件在位 → 跳过（协议 dedupe 仅 "skip"）
  PhotoRecord existing;
  std::string dbErr;
  if (photos_->getByPath(finalPath, &existing, &dbErr) && fileExists(finalPath)) {
    if (opts.verifyMd5) {
      std::string md5New;
      std::string md5Err;
      md5New = md5OfFile(finalPath, &md5Err);
      if (!md5New.empty() && md5New == existing.md5) {
        *status = "duplicate";
        return true;
      }
    } else {
      *status = "duplicate";
      return true;
    }
  }

  std::string copyErr;
  if (!copyFileTo(item.path, finalPath, &copyErr)) {
    *status = "failed";
    *error = copyErr;
    return false;
  }
  if (opts.verifyMd5) {
    std::string md5;
    std::string md5Err;
    md5 = md5OfFile(finalPath, &md5Err);
    if (md5.empty()) {
      *status = "failed";
      *error = "md5 failed: " + md5Err;
      return false;
    }
  }
  PhotoRecord record = buildRecord(opts, item, finalPath, meta);
  int64_t rootId = photos_->ensureRoot(opts.targetPath, baseName(opts.targetPath), &dbErr);
  int64_t folderId = photos_->ensureFolder(rootId, relativeDir.empty() ? std::string(".") : relativeDir,
                                           baseName(relativeDir.empty() ? opts.targetPath : relativeDir),
                                           &dbErr);
  record.rootId = rootId;
  record.folderId = folderId;
  if (opts.verifyMd5) {
    std::string md5Err;
    record.md5 = md5OfFile(finalPath, &md5Err);
  }
  if (!photos_->upsert(record, photoId, &dbErr)) {
    *status = "failed";
    *error = dbErr;
    return false;
  }
  if (opts.backupEnabled && !opts.backupTarget.empty()) {
    std::string backupDir = joinPath(opts.backupTarget, relativeDir);
    std::string backupPath = joinPath(backupDir, baseName(finalPath));
    std::string backupErr;
    if (copyFileTo(finalPath, backupPath, &backupErr)) {
      photos_->setBackup(*photoId, backupPath, nowMs(), &dbErr);
    } else {
      logWarn("import", "backup failed for " + finalPath + ": " + backupErr);
    }
  }
  return true;
}

void Importer::run(const ImportOptions& opts, JobContext& ctx) {
  int64_t imported = 0;
  int64_t duplicated = 0;
  int64_t failed = 0;
  json::JsonValue unreadable = json::JsonValue::arrayValue();

  import::ScanResult scan;
  std::string scanErr;
  bool ok = import::ImportScanner::scanFolder(
      opts.sourcePath, &scan,
      [&ctx](int percent, const std::string& detail) { ctx.progress(percent, detail); },
      [&ctx]() { return ctx.isCancelled(); }, &scanErr);
  if (!ok) {
    if (ctx.isCancelled()) return;  // 取消为正常语义
    ctx.progress(100, "scan failed: " + scanErr);
    return;
  }
  for (const auto& u : scan.unreadable) {
    json::JsonValue item = json::JsonValue::objectValue();
    item.setItem("path", u.first);
    item.setItem("error", u.second);
    unreadable.addItem(item);
  }
  int64_t total = static_cast<int64_t>(scan.items.size());
  int64_t done = 0;
  for (const ScanItem& item : scan.items) {
    if (ctx.isCancelled()) return;
    int64_t photoId = 0;
    std::string status;
    std::string error;
    bool fileOk = importOne(opts, item, &photoId, &status, &error);
    if (status == "duplicate") ++duplicated;
    else if (status == "failed") ++failed;
    else ++imported;
    ++done;
    if (notify_) {
      json::JsonValue params = json::JsonValue::objectValue();
      params.setItem("jobId", ctx.jobId());
      params.setItem("path", item.path);
      params.setItem("status", status);
      if (!error.empty()) params.setItem("error", error);
      if (photoId > 0) params.setItem("photoId", photoId);
      notify_("event.import.fileResult", params);
    }
    int percent = total > 0 ? static_cast<int>(done * 100 / total) : 100;
    ctx.progress(percent, "importing " + std::to_string(done) + "/" + std::to_string(total));
  }
  json::JsonValue byFormat = json::JsonValue::objectValue();
  for (const auto& kv : scan.byFormat) {
    byFormat.setItem(kv.first, kv.second);
  }
  json::JsonValue result = json::JsonValue::objectValue();
  result.setItem("totalFiles", scan.totalFiles);
  result.setItem("totalBytes", scan.totalBytes);
  result.setItem("byFormat", byFormat);
  result.setItem("duplicates", duplicated);
  result.setItem("rawWithJpg", scan.rawWithJpg);
  result.setItem("jpgOnly", scan.jpgOnly);
  result.setItem("unreadable", unreadable);
  result.setItem("imported", imported);
  result.setItem("failed", failed);
  // 供 JobManager 完成事件携带
  ctx.setResult(result);
  logInfo("import", "import finished: imported=" + std::to_string(imported) +
                        " duplicated=" + std::to_string(duplicated) +
                        " failed=" + std::to_string(failed));
}

}  // namespace import
}  // namespace lumina
