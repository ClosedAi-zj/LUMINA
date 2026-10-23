// 存储健康扫描实现
#include "health/scanner.h"

#include "core/logger.h"
#include "core/md5.h"
#include "core/str_util.h"

namespace lumina {
namespace health {

HealthScanner::HealthScanner(core::Config* config, db::PhotoDao* photos, db::HealthDao* health,
                             NotifyFn notify)
    : config_(config), photos_(photos), health_(health), notify_(std::move(notify)) {}

void HealthScanner::markMissing(int64_t photoId, const std::string& path) {
  std::string err;
  photos_->setMissing(photoId, true, &err);
  photos_->setHealth(photoId, "missing", &err);
  int64_t issueId = health_->recordIssue(photoId, "missing", path, path, &err);
  if (notify_) {
    json::JsonValue params = json::JsonValue::objectValue();
    params.setItem("issueId", issueId);
    params.setItem("photoId", photoId);
    params.setItem("kind", "missing");
    params.setItem("path", path);
    notify_("event.health.issue", params);
  }
}

void HealthScanner::markOk(int64_t photoId) {
  std::string err;
  photos_->setHealth(photoId, "ok", &err);
  photos_->setMissing(photoId, false, &err);
  health_->closeIssuesFor(photoId, &err);
}

void HealthScanner::runFullScan(JobContext& ctx) {
  std::string err;
  std::vector<int64_t> ids;
  if (!photos_->listIds(Filter(), &ids, &err)) {
    ctx.progress(100, "db error: " + err);
    return;
  }
  int64_t total = static_cast<int64_t>(ids.size());
  int64_t done = 0;
  int64_t missing = 0;
  for (int64_t id : ids) {
    if (ctx.isCancelled()) return;
    PhotoRecord p;
    if (!photos_->getById(id, &p, &err)) continue;
    if (fileExists(p.filePath)) {
      markOk(id);
    } else {
      markMissing(id, p.filePath);
      ++missing;
    }
    ++done;
    if (total > 0 && (done % 500 == 0)) {
      ctx.progress(static_cast<int>(done * 100 / total),
                   "checking " + std::to_string(done) + "/" + std::to_string(total));
    }
  }
  json::JsonValue result = json::JsonValue::objectValue();
  result.setItem("scanned", total);
  result.setItem("missing", missing);
  ctx.setResult(result);
  ctx.progress(100, "health scan done, missing=" + std::to_string(missing));
}

void HealthScanner::verifyPhotos(const std::vector<int64_t>& ids, JobContext& ctx) {
  int64_t total = static_cast<int64_t>(ids.size());
  int64_t done = 0;
  int64_t failed = 0;
  for (int64_t id : ids) {
    if (ctx.isCancelled()) return;
    std::string err;
    PhotoRecord p;
    if (!photos_->getById(id, &p, &err)) continue;
    if (!fileExists(p.filePath)) {
      markMissing(id, p.filePath);
      ++failed;
      ++done;
      continue;
    }
    std::string md5;
    std::string md5Err;
    md5 = md5OfFile(p.filePath, &md5Err);
    if (md5.empty()) {
      photos_->setHealth(id, "corrupt", &err);
      int64_t issueId = health_->recordIssue(id, "corrupt", p.filePath, p.filePath, &err);
      if (notify_) {
        json::JsonValue params = json::JsonValue::objectValue();
        params.setItem("issueId", issueId);
        params.setItem("photoId", id);
        params.setItem("kind", "corrupt");
        params.setItem("path", p.filePath);
        notify_("event.health.issue", params);
      }
      ++failed;
    } else if (p.md5.empty() || p.md5 == md5) {
      photos_->setMd5(id, md5, &err);
      markOk(id);
    } else {
      // 红线 R3：MD5 不符必须进隔离视图
      photos_->setHealth(id, "corrupt", &err);
      int64_t issueId = health_->recordIssue(id, "corrupt", p.filePath, p.filePath, &err);
      if (notify_) {
        json::JsonValue params = json::JsonValue::objectValue();
        params.setItem("issueId", issueId);
        params.setItem("photoId", id);
        params.setItem("kind", "corrupt");
        params.setItem("path", p.filePath);
        notify_("event.health.issue", params);
      }
      ++failed;
    }
    ++done;
    if (total > 0 && (done % 200 == 0)) {
      ctx.progress(static_cast<int>(done * 100 / total),
                   "verifying " + std::to_string(done) + "/" + std::to_string(total));
    }
  }
  json::JsonValue result = json::JsonValue::objectValue();
  result.setItem("verified", total);
  result.setItem("failed", failed);
  ctx.setResult(result);
  ctx.progress(100, "verify done, failed=" + std::to_string(failed));
}

}  // namespace health
}  // namespace lumina
