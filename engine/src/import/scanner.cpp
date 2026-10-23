// 导入前扫描实现
#include "import/scanner.h"

#include <set>

#include "core/logger.h"
#include "core/str_util.h"
#include "raw/raw_decoder.h"

namespace lumina {
namespace import {
namespace {

bool isVideoExtension(const std::string& ext) {
  static const std::set<std::string> kVideo = {"mp4", "mov", "m4v", "avi", "mpg", "mpeg", "mkv",
                                               "webm"};
  return kVideo.count(ext) > 0;
}

bool isJpegExtension(const std::string& ext) {
  return ext == "jpg" || ext == "jpeg";
}

bool isRawExtension(const std::string& ext) {
  static const std::set<std::string> kRaw = {"rw2", "dng", "arw", "cr2", "cr3", "nef",
                                             "nrw", "orf", "raf", "pef", "ptx", "r3d",
                                             "srw", "x3f", "erf", "kdc", "dcr", "raw"};
  return kRaw.count(ext) > 0;
}

}  // namespace

bool ImportScanner::scanFolder(const std::string& rootPath, ScanResult* out,
                               const ProgressFn& progress, const CancelFn& cancelled,
                               std::string* err) {
  if (!dirExists(rootPath)) {
    if (err) *err = "source folder not found: " + rootPath;
    return false;
  }
  std::vector<std::string> pending;
  pending.push_back(rootPath);
  std::map<std::string, int> pairCount;
  int processed = 0;
  while (!pending.empty()) {
    if (cancelled && cancelled()) {
      if (err) *err = "cancelled";
      return false;
    }
    std::string dir = pending.back();
    pending.pop_back();
    std::vector<DirEntry> entries;
    std::string listErr;
    if (!listDir(dir, &entries, &listErr)) {
      out->unreadable.emplace_back(dir, listErr);
      continue;
    }
    for (const DirEntry& entry : entries) {
      std::string full = joinPath(dir, entry.name);
      if (entry.isDir) {
        pending.push_back(full);
        continue;
      }
      std::string ext = extensionOf(full);
      if (ext.empty()) continue;
      if (!raw::isSupportedExtension(ext) && !isVideoExtension(ext)) continue;
      ScanItem item;
      item.path = full;
      item.fileName = entry.name;
      item.format = raw::formatOfExtension(ext);
      item.size = entry.size;
      item.mtimeMs = entry.mtimeMs;
      item.isRaw = isRawExtension(ext);
      item.isJpg = isJpegExtension(ext);
      item.isVideo = isVideoExtension(ext);
      item.pairKey = baseNameNoExt(full);
      out->items.push_back(item);
      out->totalBytes += entry.size;
      out->totalFiles += 1;
      out->byFormat[item.format] += 1;
      pairCount[item.pairKey] += 1;
      ++processed;
      if (progress && (processed % 200 == 0)) progress(50, "scanning " + std::to_string(processed));
    }
  }
  // 配对统计：RAW+JPG 同名为一条目
  for (const ScanItem& item : out->items) {
    if (pairCount[item.pairKey] > 1 && (item.isRaw || item.isJpg)) {
      out->rawWithJpg += 1;
    } else if (item.isJpg) {
      out->jpgOnly += 1;
    }
  }
  out->rawWithJpg /= 2;
  if (progress) progress(100, "scanned " + std::to_string(out->totalFiles) + " files");
  logInfo("import", "scan " + rootPath + ": " + std::to_string(out->totalFiles) + " files, " +
                        std::to_string(out->unreadable.size()) + " unreadable");
  return true;
}

}  // namespace import
}  // namespace lumina
