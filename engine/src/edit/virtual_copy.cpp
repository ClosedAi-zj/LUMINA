// 虚拟副本 / 快照持久化实现
#include "edit/virtual_copy.h"

#include "core/logger.h"
#include "core/str_util.h"

namespace lumina {
namespace edit {

VirtualCopyService::VirtualCopyService(core::Config* config, db::Database* db, db::PhotoDao* photos,
                                       xmp::XmpService* xmp)
    : config_(config), db_(db), photos_(photos), xmp_(xmp) {}

json::JsonValue VirtualCopyService::create(int64_t photoId, const std::string& name,
                                           rpc::RpcError* err) {
  PhotoRecord source;
  std::string dbErr;
  if (!photos_->getById(photoId, &source, &dbErr)) {
    *err = rpc::RpcError(rpc::kFileNotFound, "photo not found: " + std::to_string(photoId));
    return json::JsonValue::nullValue();
  }
  json::JsonValue params;
  std::string loadErr;
  if (!xmp_->loadParams(photoId, &params, &loadErr)) {
    *err = rpc::RpcError(rpc::kXmpParseError, loadErr);
    return json::JsonValue::nullValue();
  }
  // 虚拟副本：指向源文件相对路径（映射表 §5 lumina:VirtualCopyOf）
  std::string relative = baseName(source.filePath);
  std::string sql =
      "INSERT INTO virtual_copies(photo_id,name,params_json,created_at) VALUES(?,?,?,?);";
  std::string sqlErr;
  auto stmt = db_->prepareCached("vcopyCreate", sql, &sqlErr);
  if (!stmt || !stmt->valid()) {
    *err = rpc::RpcError(rpc::kDbError, sqlErr);
    return json::JsonValue::nullValue();
  }
  stmt->bindInt64(1, photoId);
  stmt->bindText(2, name.empty() ? ("虚拟副本") : name);
  stmt->bindText(3, params.serialize());
  stmt->bindInt64(4, nowMs());
  if (!stmt->step(&sqlErr)) {
    *err = rpc::RpcError(rpc::kDbError, sqlErr);
    return json::JsonValue::nullValue();
  }
  int64_t id = db_->lastInsertId();
  stmt->reset();
  // 源 sidecar 写入 lumina:VirtualCopyOf 标记（原片零写入）
  json::JsonValue marker = params;
  marker.setItem("virtualCopyOf", relative);
  std::string saveErr;
  if (!xmp_->saveParams(photoId, marker, &saveErr)) {
    logWarn("edit", "vcopy marker write skipped: " + saveErr);
  }
  logInfo("edit", "virtual copy " + std::to_string(id) + " created from photo " +
                      std::to_string(photoId));
  json::JsonValue out = json::JsonValue::objectValue();
  out.setItem("vcopyId", id);
  return out;
}

json::JsonValue VirtualCopyService::list(int64_t photoId, rpc::RpcError* err) {
  std::string sql = "SELECT id, name, created_at FROM virtual_copies WHERE photo_id=? ORDER BY "
                    "created_at DESC;";
  std::string sqlErr;
  auto stmt = db_->prepareCached("vcopyList", sql, &sqlErr);
  if (!stmt || !stmt->valid()) {
    *err = rpc::RpcError(rpc::kDbError, sqlErr);
    return json::JsonValue::nullValue();
  }
  stmt->bindInt64(1, photoId);
  json::JsonValue out = json::JsonValue::arrayValue();
  while (stmt->step(&sqlErr)) {
    json::JsonValue item = json::JsonValue::objectValue();
    item.setItem("vcopyId", stmt->columnInt64(0));
    item.setItem("name", stmt->columnText(1));
    item.setItem("createdAt", stmt->columnInt64(2));
    out.addItem(item);
  }
  stmt->reset();
  return out;
}

}  // namespace edit
}  // namespace lumina
