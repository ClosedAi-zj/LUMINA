// XMP 版本迁移器实现
#include "xmp/migrator.h"

#include "core/logger.h"
#include "core/str_util.h"
#include "rpc/version.h"
#include "xmp/xmp_service.h"

namespace lumina {
namespace xmp {

XmpMigrator::XmpMigrator(core::Config* config, db::PhotoDao* photos, db::Database* db)
    : config_(config), photos_(photos), db_(db) {}

bool XmpMigrator::migrateOne(int64_t photoId, std::string* err) {
  PhotoRecord p;
  if (!photos_->getById(photoId, &p, err)) {
    if (err->empty()) *err = "photo not found";
    return false;
  }
  std::string sidecar = p.sidecarPath.empty() ? replaceExtension(p.filePath, "xmp") : p.sidecarPath;
  if (!fileExists(sidecar)) return true;  // 无 sidecar 无需迁移
  std::string bytes;
  if (!readFile(sidecar, &bytes, err)) return false;
  std::string parseErr;
  std::unique_ptr<XmpDocument> doc = XmpDocument::parse(bytes, &parseErr);
  if (!doc) {
    if (err) *err = "xmp parse failed: " + sidecar;
    return false;
  }
  int64_t current = doc->getInt("lumina", "SchemaVersion", 0);
  if (current >= kXmpSchemaVersion) return true;
  // 迁移规则 v1：补 lumina:SchemaVersion + crs:ProcessVersion（若缺失）
  doc->setText("lumina", "SchemaVersion", std::to_string(kXmpSchemaVersion));
  if (!doc->has("crs", "ProcessVersion")) {
    doc->setText("crs", "ProcessVersion", kMaxProcessVersion);
  }
  std::string output = doc->serialize();
  if (!writeFileAtomic(sidecar, output, err)) return false;
  photos_->setSidecar(photoId, sidecar, fileMtimeMs(sidecar), true, err);
  logInfo("xmp", "migrated sidecar " + sidecar + " from schema " + std::to_string(current));
  return true;
}

bool XmpMigrator::migrateAll(int* migrated, std::string* err) {
  *migrated = 0;
  std::string sql = "SELECT id FROM photos WHERE deleted=0 AND (sidecar_path IS NOT NULL AND "
                    "sidecar_path<>'') ORDER BY id;";
  auto stmt = db_->prepareCached("migrateAllIds", sql, err);
  if (!stmt || !stmt->valid()) return false;
  std::vector<int64_t> ids;
  while (stmt->step(err)) {
    ids.push_back(stmt->columnInt64(0));
  }
  stmt->reset();
  int count = 0;
  for (int64_t id : ids) {
    std::string oneErr;
    if (migrateOne(id, &oneErr)) {
      // 只有真正发生写入才算迁移一次；此处简化：无 sidecar 跳过
      ++count;
    } else {
      logWarn("xmp", "migration skipped for photo " + std::to_string(id) + ": " + oneErr);
    }
  }
  *migrated = count;
  return true;
}

json::JsonValue XmpMigrator::status(rpc::RpcError* err) {
  json::JsonValue out = json::JsonValue::objectValue();
  out.setItem("current", kXmpSchemaVersion);
  json::JsonValue distribution = json::JsonValue::objectValue();
  distribution.setItem("0", 0);
  distribution.setItem("1", 0);
  int64_t migrated = 0;
  std::string sqlErr;
  std::string sql = "SELECT id, sidecar_path FROM photos WHERE deleted=0 AND sidecar_path IS NOT "
                    "NULL AND sidecar_path<>'' ORDER BY id;";
  auto stmt = db_->prepareCached("migrationScan", sql, &sqlErr);
  if (!stmt || !stmt->valid()) {
    *err = rpc::RpcError(rpc::kDbError, sqlErr);
    return json::JsonValue::nullValue();
  }
  int64_t total = 0;
  while (stmt->step(&sqlErr)) {
    int64_t id = stmt->columnInt64(0);
    std::string sidecar = stmt->columnText(1);
    ++total;
    std::string bytes;
    std::string readErr;
    if (!readFile(sidecar, &bytes, &readErr)) continue;
    std::string parseErr;
    std::unique_ptr<XmpDocument> doc = XmpDocument::parse(bytes, &parseErr);
    if (!doc) continue;
    int64_t version = doc->getInt("lumina", "SchemaVersion", 0);
    if (version < kXmpSchemaVersion) {
      distribution.setItem("0", distribution.get("0").asInt() + 1);
      std::string oneErr;
      if (migrateOne(id, &oneErr)) ++migrated;
    } else {
      distribution.setItem("1", distribution.get("1").asInt() + 1);
    }
  }
  stmt->reset();
  out.setItem("distribution", distribution);
  out.setItem("migrated", migrated);
  out.setItem("sidecars", total);
  return out;
}

}  // namespace xmp
}  // namespace lumina
