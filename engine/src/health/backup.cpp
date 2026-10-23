// 备份与恢复实现
#include "health/backup.h"

#include <algorithm>
#include <map>

#include "core/logger.h"
#include "core/md5.h"
#include "core/str_util.h"

namespace lumina {
namespace health {

BackupService::BackupService(core::Config* config, db::PhotoDao* photos, db::Database* db,
                             NotifyFn notify)
    : config_(config), photos_(photos), db_(db), notify_(std::move(notify)) {}

void BackupService::runBackup(JobContext& ctx) {
  std::string target = config_->backupTarget();
  if (target.empty()) {
    ctx.progress(100, "backup target not configured");
    return;
  }
  std::string err;
  std::vector<int64_t> ids;
  if (!photos_->listIds(Filter(), &ids, &err)) {
    ctx.progress(100, "db error: " + err);
    return;
  }
  int64_t total = static_cast<int64_t>(ids.size());
  int64_t done = 0;
  int64_t backedUp = 0;         // 本次实际复制的文件数（WP-A6 增量语义）
  int64_t skippedVerified = 0;  // 存在+大小+MD5 一致而跳过的文件数
  for (int64_t id : ids) {
    if (ctx.isCancelled()) return;
    PhotoRecord p;
    if (!photos_->getById(id, &p, &err)) continue;
    if (!fileExists(p.filePath)) continue;
    // 保持相对结构：{target}/{rootId}/{fileName}（与 health.exportDiffReport 同一约定）
    std::string backupDir = joinPath(target, std::to_string(p.rootId));
    std::string backupPath = joinPath(backupDir, p.fileName);
    std::string copyErr;
    // WP-A6 增量语义：存在 + 大小一致后再比 MD5 —— 库里有已校验的 md5 就直接比备份文件，
    // 否则源/备份各算一次。MD5 一致才跳过（旧实现只比大小，同大小不同内容会漏检）。
    bool needCopy = true;
    if (fileExists(backupPath) && fileSize(backupPath) == p.fileSize) {
      std::string md5Err;
      std::string srcMd5 = p.md5;
      if (srcMd5.empty()) srcMd5 = md5OfFile(p.filePath, &md5Err);
      std::string bakMd5 = srcMd5.empty() ? std::string() : md5OfFile(backupPath, &md5Err);
      if (!srcMd5.empty() && srcMd5 == bakMd5) needCopy = false;
    }
    if (!needCopy) {
      // 已备份且校验一致：仅刷新备份登记时间戳
      photos_->setBackup(id, backupPath, nowMs(), &err);
      ++skippedVerified;
    } else if (copyFileTo(p.filePath, backupPath, &copyErr)) {
      std::string sidecar = p.sidecarPath.empty() ? replaceExtension(p.filePath, "xmp")
                                                  : p.sidecarPath;
      if (fileExists(sidecar)) {
        std::string sidecarBackup = replaceExtension(backupPath, "xmp");
        copyFileTo(sidecar, sidecarBackup, &copyErr);
      }
      photos_->setBackup(id, backupPath, nowMs(), &err);
      ++backedUp;
    } else {
      logWarn("backup", "backup failed for " + p.filePath + ": " + copyErr);
    }
    ++done;
    if (total > 0 && (done % 500 == 0)) {
      ctx.progress(static_cast<int>(done * 100 / total),
                   "backing up " + std::to_string(done) + "/" + std::to_string(total));
    }
  }
  json::JsonValue result = json::JsonValue::objectValue();
  result.setItem("backedUp", backedUp);
  result.setItem("skippedVerified", skippedVerified);
  result.setItem("total", total);
  ctx.setResult(result);
  ctx.progress(100, "backup done, copied=" + std::to_string(backedUp) + " skipped=" +
                        std::to_string(skippedVerified));
}

bool BackupService::restorePhoto(int64_t photoId, std::string* restoredPath, std::string* err) {
  PhotoRecord p;
  if (!photos_->getById(photoId, &p, err)) {
    if (err->empty()) *err = "photo not found";
    return false;
  }
  if (fileExists(p.filePath)) {
    *restoredPath = p.filePath;
    return true;  // 已在位
  }
  std::string sql = "SELECT backup_path FROM backups WHERE photo_id=?;";
  auto stmt = db_->prepareCached("backupGet", sql, err);
  if (!stmt || !stmt->valid()) return false;
  stmt->bindInt64(1, photoId);
  if (!stmt->step(err)) {
    *err = "no backup record for photo " + std::to_string(photoId);
    stmt->reset();
    return false;
  }
  std::string backupPath = stmt->columnText(0);
  stmt->reset();
  if (!fileExists(backupPath)) {
    *err = "backup file missing: " + backupPath;
    return false;
  }
  if (!copyFileTo(backupPath, p.filePath, err)) return false;
  std::string sidecarBackup = replaceExtension(backupPath, "xmp");
  if (fileExists(sidecarBackup)) {
    std::string sidecar = replaceExtension(p.filePath, "xmp");
    std::string copyErr;
    copyFileTo(sidecarBackup, sidecar, &copyErr);
  }
  std::string dbErr;
  photos_->setMissing(photoId, false, &dbErr);
  photos_->setHealth(photoId, "ok", &dbErr);
  *restoredPath = p.filePath;
  logInfo("backup", "restored photo " + std::to_string(photoId) + " from " + backupPath);
  return true;
}

json::JsonValue BackupService::configJson() {
  json::JsonValue out = json::JsonValue::objectValue();
  json::JsonValue settings = config_->settings();
  out.setItem("enabled", settings.get("backupEnabled").asBool(false));
  out.setItem("targetPath", settings.get("backupTarget").asString());
  out.setItem("timing", settings.get("backupTiming").asString("onImport"));
  out.setItem("lastRunAt", json::JsonValue::nullValue());
  return out;
}

// ---- WP-A5：双备份差异报告导出（@since 1.1.0） ----
namespace {

// CSV 字段转义：含逗号/引号/换行时包引号并双写引号
std::string csvField(const std::string& v) {
  if (v.find_first_of(",\"\n\r") == std::string::npos) return v;
  std::string out = "\"";
  for (char c : v) {
    if (c == '"') out += "\"\"";
    else out += c;
  }
  out += "\"";
  return out;
}

// 递归收集目录下所有文件（返回相对路径，POSIX 分隔）
void collectFiles(const std::string& dir, const std::string& relPrefix,
                  std::map<std::string, std::string>* out) {
  std::vector<DirEntry> entries;
  std::string err;
  if (!listDir(dir, &entries, &err)) return;
  for (const DirEntry& e : entries) {
    std::string rel = relPrefix.empty() ? e.name : relPrefix + "/" + e.name;
    if (e.isDir) {
      collectFiles(joinPath(dir, e.name), rel, out);
    } else {
      (*out)[rel] = joinPath(dir, e.name);
    }
  }
}

}  // namespace

json::JsonValue BackupService::exportDiffReport(const json::JsonValue& params,
                                                rpc::RpcError* err) {
  const std::string format = params.get("format").asString("json");
  if (format != "csv" && format != "json") {
    *err = rpc::RpcError(rpc::kInvalidParams, "params.format must be \"csv\" or \"json\"");
    return json::JsonValue::nullValue();
  }
  const std::string target = config_->backupTarget();
  if (target.empty()) {
    *err = rpc::RpcError(rpc::kInvalidParams,
                         "备份根未配置（system.setSettings backupTarget），无法比对");
    return json::JsonValue::nullValue();
  }
  if (!dirExists(target)) {
    *err = rpc::RpcError(rpc::kFileNotFound, "备份根目录不存在: " + target);
    return json::JsonValue::nullValue();
  }

  // 1) 库内照片 → 期望备份路径（布局 {target}/{rootId}/{fileName}，与 runBackup 一致）。
  //    原片本就缺失的照片不参与比对（备份无法凭空产生，属于 health.scanFull 的管辖）。
  struct Item {
    int64_t photoId = 0;
    std::string path;       // 库内路径（missing 时）或备份路径（extra 时）
    std::string diffType;   // missing | extra | checksum_failed
    std::string status;     // missing | extra | size_mismatch | md5_mismatch
    std::string detail;
  };
  std::vector<Item> items;
  std::map<std::string, PhotoRecord> expected;  // 备份相对路径 → 照片
  {
    std::vector<int64_t> ids;
    std::string listErr;
    if (!photos_->listIds(Filter(), &ids, &listErr)) {
      *err = rpc::RpcError(rpc::kDbError, listErr);
      return json::JsonValue::nullValue();
    }
    for (int64_t id : ids) {
      PhotoRecord p;
      std::string getErr;
      if (!photos_->getById(id, &p, &getErr)) continue;
      if (!fileExists(p.filePath)) continue;
      std::string rel = std::to_string(p.rootId) + "/" + p.fileName;
      expected[rel] = p;
    }
  }

  // 2) 扫描备份根实际文件
  std::map<std::string, std::string> actual;  // 相对路径 → 绝对路径
  collectFiles(target, "", &actual);

  // 3) 三类差异（零静默：每一处差异都成行）
  for (const auto& kv : expected) {
    const PhotoRecord& p = kv.second;
    auto it = actual.find(kv.first);
    if (it == actual.end()) {
      Item item;
      item.photoId = p.id;
      item.path = p.filePath;
      item.diffType = "missing";
      item.status = "missing";
      item.detail = "备份缺失: 期望 " + joinPath(target, kv.first);
      items.push_back(item);
      continue;
    }
    const std::string backupPath = it->second;
    const int64_t backupSize = fileSize(backupPath);
    if (backupSize != p.fileSize) {
      Item item;
      item.photoId = p.id;
      item.path = p.filePath;
      item.diffType = "checksum_failed";
      item.status = "size_mismatch";
      item.detail = "大小不符: 库 " + std::to_string(p.fileSize) + "B vs 备份 " +
                    std::to_string(backupSize) + "B";
      items.push_back(item);
    } else if (!p.md5.empty()) {
      // 库里有已校验的 MD5 时才算备份文件 MD5（避免无谓的全量哈希）
      std::string md5Err;
      std::string backupMd5 = md5OfFile(backupPath, &md5Err);
      if (backupMd5.empty()) {
        Item item;
        item.photoId = p.id;
        item.path = p.filePath;
        item.diffType = "checksum_failed";
        item.status = "md5_mismatch";
        item.detail = "备份文件 MD5 计算失败: " + md5Err;
        items.push_back(item);
      } else if (backupMd5 != p.md5) {
        Item item;
        item.photoId = p.id;
        item.path = p.filePath;
        item.diffType = "checksum_failed";
        item.status = "md5_mismatch";
        item.detail = "MD5 不符: 库 " + p.md5 + " vs 备份 " + backupMd5;
        items.push_back(item);
      }
    }
  }
  for (const auto& kv : actual) {
    if (expected.count(kv.first)) continue;
    Item item;
    item.photoId = 0;
    item.path = kv.second;
    item.diffType = "extra";
    item.status = "extra";
    item.detail = "备份多余文件: 库中无对应照片（" + kv.first + "）";
    items.push_back(item);
  }

  // 4) 落盘（用户可选目录，默认 {home}/reports/）
  std::string outPath = params.get("path").asString();
  if (outPath.empty()) {
    std::string stamp = formatTimestamp(nowMs());
    // "YYYY-MM-DD HH:MM:SS.mmm" → YYYYMMDD-HHMMSS
    std::string day = stamp.substr(0, 10);
    day.erase(std::remove(day.begin(), day.end(), '-'), day.end());
    std::string hms = stamp.substr(11, 8);
    hms.erase(std::remove(hms.begin(), hms.end(), ':'), hms.end());
    outPath = joinPath(config_->homeDir(), "reports/diff-" + day + "-" + hms + "." + format);
  } else if (outPath.find('.') == std::string::npos) {
    outPath += "." + format;
  }
  std::string dir = outPath;
  size_t slash = dir.find_last_of("/\\");
  if (slash != std::string::npos) dir = dir.substr(0, slash);
  if (!dirExists(dir) && !makeDirs(dir)) {
    *err = rpc::RpcError(rpc::kIoError, "无法创建报告目录: " + dir);
    return json::JsonValue::nullValue();
  }

  std::string content;
  if (format == "csv") {
    content += "photoId,path,diffType,status,detail\n";
    for (const Item& it : items) {
      content += csvField(std::to_string(it.photoId)) + "," + csvField(it.path) + "," +
                 csvField(it.diffType) + "," + csvField(it.status) + "," +
                 csvField(it.detail) + "\n";
    }
  } else {
    json::JsonValue report = json::JsonValue::objectValue();
    report.setItem("schema", "lumina.health.diffReport/1");
    report.setItem("generatedAt", nowMs());
    report.setItem("backupTarget", target);
    report.setItem("count", static_cast<int64_t>(items.size()));
    json::JsonValue arr = json::JsonValue::arrayValue();
    for (const Item& it : items) {
      json::JsonValue o = json::JsonValue::objectValue();
      o.setItem("photoId", it.photoId);
      o.setItem("path", it.path);
      o.setItem("diffType", it.diffType);
      o.setItem("status", it.status);
      o.setItem("detail", it.detail);
      arr.addItem(o);
    }
    report.setItem("items", arr);
    content = report.serialize();
  }
  std::string writeErr;
  if (!writeFileAtomic(outPath, content, &writeErr)) {
    *err = rpc::RpcError(rpc::kIoError, "报告写盘失败: " + writeErr);
    return json::JsonValue::nullValue();
  }
  logInfo("backup", "diff report exported: " + outPath + " (" + std::to_string(items.size()) +
                        " items)");
  json::JsonValue out = json::JsonValue::objectValue();
  out.setItem("path", outPath);
  out.setItem("count", static_cast<int64_t>(items.size()));
  return out;
}

}  // namespace health
}  // namespace lumina
