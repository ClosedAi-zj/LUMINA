// 导入执行实现
#include "import/importer.h"

#include <set>
#include <utility>

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

/** 读文件头部 n 字节（宽字符路径，含中文的目录/文件名可用）。 */
bool readHead(const std::string& path, size_t n, std::string* out) {
#ifdef _WIN32
  FILE* f = _wfopen(utf8ToUtf16(path).c_str(), L"rb");
#else
  FILE* f = fopen(path.c_str(), "rb");
#endif
  if (!f) return false;
  out->assign(n, '\0');
  size_t got = fread(&(*out)[0], 1, n, f);
  fclose(f);
  out->resize(got);
  return got > 0;
}

/**
 * PNG 像素尺寸：只读 24 字节文件头。
 * 布局：0..7 签名 · 8..11 首块长度(0x0D) · 12..15 块类型("IHDR") · 16..19 宽 · 20..23 高（均大端）。
 * 之前导入链只对 RAW/JPEG/视频探测元数据，PNG 入库后 width/height 恒为 0
 * （图库卡片、详情、导出长边计算全部受影响），用户导入 PNG 会看到 0×0。
 */
bool probePngSize(const std::string& path, int* width, int* height) {
  std::string head;
  if (!readHead(path, 24, &head) || head.size() < 24) return false;
  static const unsigned char kSignature[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
  const unsigned char* p = reinterpret_cast<const unsigned char*>(head.data());
  for (int i = 0; i < 8; ++i) {
    if (p[i] != kSignature[i]) return false;
  }
  if (head.compare(12, 4, "IHDR") != 0) return false;
  uint32_t w = (static_cast<uint32_t>(p[16]) << 24) | (static_cast<uint32_t>(p[17]) << 16) |
               (static_cast<uint32_t>(p[18]) << 8) | static_cast<uint32_t>(p[19]);
  uint32_t h = (static_cast<uint32_t>(p[20]) << 24) | (static_cast<uint32_t>(p[21]) << 16) |
               (static_cast<uint32_t>(p[22]) << 8) | static_cast<uint32_t>(p[23]);
  if (w == 0 || h == 0 || w > 200000 || h > 200000) return false;
  *width = static_cast<int>(w);
  *height = static_cast<int>(h);
  return true;
}

/**
 * 目标同名保护：绝不覆盖已存在的文件。
 *
 * 触发场景（实测）：重命名模板若把所有文件算成同一个基名（例如模板里没有任何
 * {token}，或相机字段为空且原名重复），同一目录下所有照片会落到同一个目标路径；
 * CopyFileW(bFailIfExists=FALSE) 会静默覆盖前一张，用户看到的是"导入了 N 张，
 * 库里只有 1 张"——即"无法导入自己的文件"。这里改成 -2/-3… 递增，保证零丢失。
 */
std::string uniqueTargetPath(const std::string& path) {
  if (!fileExists(path)) return path;
  const std::string dir = parentPath(path);
  const std::string base = baseNameNoExt(path);
  const std::string ext = extensionOf(path);
  for (int i = 2; i < 100000; ++i) {
    const std::string candidate = joinPath(dir, base + "-" + std::to_string(i) + "." + ext);
    if (!fileExists(candidate)) return candidate;
  }
  return path;  // 极端情况（10 万个同名）：交给调用方按失败处理
}

}  // namespace

Importer::Importer(core::Config* config, db::PhotoDao* photos, db::ImageDao* images,
                   db::Database* db, db::FtsIndexer* fts, raw::RawDecoder* decoder,
                   xmp::XmpService* xmp, NotifyFn notify)
    : config_(config), photos_(photos), images_(images), db_(db), fts_(fts), decoder_(decoder),
      xmp_(xmp), notify_(std::move(notify)) {
  if (!images_) {
    // 零静默：身份层缺失是装配错误，必须可见（功能退化为路径+MD5 判重）
    logWarn("import", "ImageDao not wired; file-identity dedupe degraded to path+md5 only");
  }
}

// 内容级去重（协议 §5.8「MD5 相同默认跳过并计数」）：库内任意照片 md5 相同即视为重复。
// 只按目标路径判重是不够的：同一张照片改了名、或重命名模板把多张算成同名，
// 都会绕过路径判重而被重复拷贝或误判。
bool Importer::findPhotoIdByMd5(const std::string& md5, int64_t* photoId) {
  if (!db_ || md5.empty()) return false;
  std::string sqlErr;
  auto stmt = db_->prepareCached(
      "findPhotoByMd5", "SELECT id FROM photos WHERE md5=? AND deleted=0 LIMIT 1;", &sqlErr);
  if (!stmt || !stmt->valid()) {
    logWarn("import", "md5 dedupe query failed: " + sqlErr);
    return false;
  }
  stmt->bindText(1, md5);
  bool found = stmt->step(&sqlErr) && !stmt->columnIsNull(0);
  if (found && photoId) *photoId = stmt->columnInt64(0);
  stmt->reset();
  return found;
}

// 元数据探测（RAW 走 LibRaw，JPEG/PNG 走 EXIF，视频走容器探测）。
// importOne 在重命名之前就要用（拍摄时间/机型进文件名），故与 buildRecord 分离、
// 整个导入只探测一次，不重复打开文件。

// 轻量 JPEG 尺寸探测：只扫 SOF0/1/2 标记读宽高（不解码像素，微秒级）
bool probeJpegSofSize(const std::string& path, int* width, int* height) {
  std::string bytes;
  std::string readErr;
  if (!readFile(path, &bytes, &readErr) || bytes.size() < 16) return false;
  const auto* d = reinterpret_cast<const unsigned char*>(bytes.data());
  const size_t n = bytes.size();
  size_t i = 2;
  while (i + 9 < n) {
    if (d[i] != 0xFF) { ++i; continue; }
    const unsigned char m = d[i + 1];
    if (m == 0xC0 || m == 0xC1 || m == 0xC2 || m == 0xC3) {
      if (i + 9 > n) return false;
      *height = (d[i + 5] << 8) | d[i + 6];
      *width = (d[i + 7] << 8) | d[i + 8];
      return *width > 0 && *height > 0;
    }
    if (m == 0xD8 || m == 0x01 || (m >= 0xD0 && m <= 0xD7)) { i += 2; continue; }
    if (i + 4 > n) return false;
    const uint16_t len = static_cast<uint16_t>((d[i + 2] << 8) | d[i + 3]);
    if (len < 2) return false;
    i += 2 + len;
  }
  return false;
}

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
      // 竖拍修正：orientation≥5 表示显示需旋转，入库宽高必须是显示方向
      // （实测：PANA 竖拍 JPG orientation=8，入库写成 6000×4000 横向，UI 全按横图渲染）
      if (exif.orientation >= 5 && meta->width > 0 && meta->height > 0) {
        std::swap(meta->width, meta->height);
      }
      // EXIF 像素尺寸缺失/解析失败（实测 PANA JPG 两项皆空 → 入库 0×0）：
      // 轻量扫 SOF 标记拿真实尺寸，只读头部不解码
      if (meta->width <= 0 || meta->height <= 0) {
        int w = 0;
        int h = 0;
        if (probeJpegSofSize(item.path, &w, &h) && w > 0 && h > 0) {
          if (exif.orientation >= 5) std::swap(w, h);
          meta->width = w;
          meta->height = h;
        }
      }
    }
  } else if (item.isVideo) {
    metadata::VideoInfo video;
    std::string err;
    if (metadata::probeVideo(item.path, &video, &err)) {
      meta->width = video.width;
      meta->height = video.height;
      meta->timestampMs = video.creationMs;
    }
  } else if (item.format == "PNG") {
    // PNG 无 EXIF 相机/时间信息，但尺寸必须拿到（否则入库 0×0）
    int w = 0;
    int h = 0;
    if (probePngSize(item.path, &w, &h)) {
      meta->width = w;
      meta->height = h;
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
  // T2.12 修复（migration 005 成因）：此前对所有导入行无条件写虚构 sidecar_path
  // （无论 .xmp 是否存在），存量库因此积累 13,842 行「路径非空且文件不存在」的假失效行。
  // 只在 sidecar 真实存在时登记；未编辑照片保持空（=「本来无 sidecar」，读取路径
  // 对空值有 sidecarPathFor 重算兜底），首次编辑时由 updateSidecar 写入真实路径。
  const std::string sidecarCandidate = replaceExtension(finalPath, "xmp");
  if (fileExists(sidecarCandidate)) p.sidecarPath = sidecarCandidate;
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

  // 去重：同路径已在索引且文件在位 → 按 MD5 判断是否真的是同一张。
  // 注意必须比**源文件**的 MD5 与库内记录：原实现比的是目标文件自身的 MD5，
  // 那个值恒等于库内记录，于是任何"重命名后同名"的不同照片都会被误判为
  // duplicate 静默丢弃（实测：2 张不同照片 → 只入库 1 张）。
  // 源文件 MD5：去重要用它（比的是"源内容 vs 库内内容"），后面写库时直接复用，
  // 避免对同一张照片读两遍。
  std::string sourceMd5;
  if (opts.verifyMd5) {
    std::string md5Err;
    sourceMd5 = md5OfFile(item.path, &md5Err);
    if (sourceMd5.empty()) {
      *status = "failed";
      *error = "md5 failed: " + md5Err;
      return false;
    }
  }

  // ---- 库身份准备（M36：root/folder/files 行在此落位，判重与落库共用） ----
  std::string dbErr;
  const std::string relForDb = relativeDir.empty() ? std::string(".") : relativeDir;
  const int64_t rootId = photos_->ensureRoot(opts.targetPath, baseName(opts.targetPath), &dbErr);
  const int64_t folderId =
      photos_->ensureFolder(rootId, relForDb,
                            baseName(relForDb.empty() ? opts.targetPath : relForDb), &dbErr);

  // 去重 ①：同路径已在索引且文件在位 → 按 MD5 判断是否真的是同一张。
  // 注意必须比**源文件**的 MD5 与库内记录：原实现比的是目标文件自身的 MD5，
  // 那个值恒等于库内记录，于是任何"重命名后同名"的不同照片都会被误判为
  // duplicate 静默丢弃（实测：2 张不同照片 → 只入库 1 张）。
  PhotoRecord existing;
  if (photos_->getByPath(finalPath, &existing, &dbErr) && fileExists(finalPath)) {
    bool sameContent = true;
    if (opts.verifyMd5) {
      sameContent = !existing.md5.empty() && sourceMd5 == existing.md5;
    }
    if (sameContent) {
      *status = "duplicate";
      if (photoId) *photoId = existing.id;
      return true;
    }
    // 同名但内容不同 → 落到下面的改名逻辑，绝不丢照片
    logInfo("import", "same target name but different content: " + finalPath);
  }

  // 去重 ②：内容级 —— 同一张照片换个名字/换个目录再导入（或模板把多张算成同名）时，
  // 仅按目标路径判重会漏掉，导致库里出现重复条目。
  // 附加语义（架构 T2.2b 验收①）：若原文件已经不在盘上（用户在库外移动/重命名了
  // 文件夹），视为「同一逻辑文件被移动」——把既有照片身份 relocate 到新位置并补拷
  // 文件（photos.id 不变、files.folder_id 跟随新位置、sidecar_path 重算），而不是
  // 另建新行或静默跳过。
  if (opts.verifyMd5) {
    int64_t dupId = 0;
    if (findPhotoIdByMd5(sourceMd5, &dupId) && dupId > 0) {
      PhotoRecord dupRec;
      const bool fileStillThere =
          photos_->getById(dupId, &dupRec, &dbErr) && fileExists(dupRec.filePath);
      const bool nameStable =
          dupRec.fileName == baseName(finalPath);  // 改名导入 ≠ 移动，不强行搬迁
      if (fileStillThere || !images_ || !nameStable || dupRec.deleted) {
        *status = "duplicate";
        if (photoId) *photoId = dupId;
        return true;
      }
      // 文件已离盘 → 搬迁身份到本次目标位置，再确保文件落位
      if (!images_->relocate(dupId, rootId, relForDb, &dbErr)) {
        *status = "failed";
        *error = "relocate after move detected: " + dbErr;
        return false;
      }
      std::string copyErr;
      if (!copyFileTo(item.path, finalPath, &copyErr)) {
        *status = "failed";
        *error = copyErr;
        return false;
      }
      if (photoId) *photoId = dupId;
      logInfo("import", "re-linked moved photo " + std::to_string(dupId) + " -> " + finalPath);
      return true;  // 身份已搬迁（relocate 内含 sidecar 重算），无需再 upsert
    }
  }

  // 同名冲突保护：走到这里说明目标路径上的既有文件不是"同内容重复"
  // （同内容在上面已经按 duplicate 跳过）。直接写会静默覆盖，故先改名。
  const std::string desiredPath = finalPath;
  finalPath = uniqueTargetPath(finalPath);
  if (finalPath != desiredPath) {
    logWarn("import", "target name collision, renamed: " + desiredPath + " -> " + finalPath);
  }

  // 去重 ③（I1）：文件身份判重 —— files(folder_id, name) 已绑定的照片直接复用。
  // 覆盖 verifyMd5=false 的导入路径：同一逻辑文件（换模板/换目录）不再产生新 photos.id。
  int64_t fileId = 0;
  if (images_) {
    fileId = images_->ensureFileRow(folderId, baseName(finalPath), &dbErr);
    if (fileId <= 0) {
      *status = "failed";
      *error = "ensureFileRow failed: " + dbErr;
      return false;
    }
    const int64_t byFile = images_->photoIdByFile(fileId, &dbErr);
    if (byFile > 0) {
      PhotoRecord byFileRec;
      if (photos_->getById(byFile, &byFileRec, &dbErr) && fileExists(byFileRec.filePath)) {
        *status = "duplicate";
        if (photoId) *photoId = byFile;
        return true;
      }
      // 该文件身份已绑定但文件不在盘上（被库外删除）→ 重新落盘 + 原位 upsert
      // （record.fileId>0 → upsert 按 file_id 认行，photos.id 保持不变）
      logInfo("import", "file identity " + std::to_string(fileId) +
                            " already bound to missing photo " + std::to_string(byFile) +
                            ", restoring in place");
      if (photoId) *photoId = byFile;
    }
  }

  std::string copyErr;
  if (!copyFileTo(item.path, finalPath, &copyErr)) {
    *status = "failed";
    *error = copyErr;
    return false;
  }
  PhotoRecord record = buildRecord(opts, item, finalPath, meta);
  record.rootId = rootId;
  record.folderId = folderId;
  record.fileId = fileId;  // M36：落库前建档（files 行已在上面 ensure）
  record.md5 = sourceMd5;
  if (!photos_->upsert(record, photoId, &dbErr)) {
    *status = "failed";
    *error = dbErr;
    return false;
  }
  if (images_ && fileId > 0 && record.fileId != fileId) {
    // upsert 走了插入路径且未带上 file_id（理论不可达）→ 立即补绑定，零静默
    if (!images_->linkPhoto(*photoId, fileId, &dbErr)) {
      logWarn("import", "post-link failed for photo " + std::to_string(*photoId) + ": " + dbErr);
    }
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
    // 扫描失败不能静默（红线 R3）：只发一条 progress 就收工，UI 拿不到任何原因，
    // 用户看到的是"点了导入什么也没发生"。这里显式落 error 字段供 UI 弹窗展示。
    logError("import", "scan failed for " + opts.sourcePath + ": " + scanErr);
    json::JsonValue failure = json::JsonValue::objectValue();
    failure.setItem("totalFiles", 0);
    failure.setItem("totalBytes", 0);
    failure.setItem("byFormat", json::JsonValue::objectValue());
    failure.setItem("duplicates", 0);
    failure.setItem("rawWithJpg", 0);
    failure.setItem("jpgOnly", 0);
    failure.setItem("unreadable", json::JsonValue::arrayValue());
    failure.setItem("imported", 0);
    failure.setItem("failed", 0);
    failure.setItem("error", "扫描失败：" + scanErr);
    // 必须走 fail() 而不是 setResult()：setResult 只会挂上结果，
    // JobManager 的终态判定仍会给出 done —— 源目录不可访问也报「导入完成」。
    ctx.fail(failure);
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
  // 零文件 / 全失败必须带可读原因（R3：结果显式告知，禁止"看起来成功了但什么都没有"）
  if (total == 0) {
    result.setItem("error",
                   "源目录中没有可导入的照片（支持 RAW/JPG/PNG/TIFF/BMP/WEBP 与常见视频）：" +
                       opts.sourcePath);
  } else if (imported == 0 && failed > 0) {
    result.setItem("error", "全部 " + std::to_string(failed) +
                                " 个文件导入失败，首个原因见失败清单与日志");
  }
  // 供 JobManager 完成事件携带
  ctx.setResult(result);
  logInfo("import", "import finished: imported=" + std::to_string(imported) +
                        " duplicated=" + std::to_string(duplicated) +
                        " failed=" + std::to_string(failed));
}

}  // namespace import
}  // namespace lumina
