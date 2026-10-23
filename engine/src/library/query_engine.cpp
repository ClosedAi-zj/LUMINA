// 查询引擎实现
#include "library/query_engine.h"

#include "core/logger.h"
#include "core/str_util.h"

namespace lumina {
namespace library {
namespace {

std::vector<int64_t> parseIdList(const json::JsonValue& v) {
  std::vector<int64_t> out;
  if (v.isArray()) {
    for (const auto& item : v.arrayItems()) out.push_back(item.asInt());
  } else if (v.isNumber()) {
    out.push_back(v.asInt());
  }
  return out;
}

}  // namespace

json::JsonValue QueryEngine::queryPhotos(const json::JsonValue& params, rpc::RpcError* err) {
  Filter filter;
  std::string filterErr;
  if (!filterFromJson(params.get("filter"), &filter, &filterErr)) {
    *err = rpc::RpcError(rpc::kInvalidParams, filterErr);
    return json::JsonValue::nullValue();
  }
  Sort sort;
  sortFromJson(params.get("sort"), &sort);
  Page page;
  pageFromJson(params.get("page"), &page);

  QueryResult result;
  std::string dbErr;
  if (!photos_->query(filter, sort, page, &result, &dbErr)) {
    *err = rpc::RpcError(rpc::kDbError, dbErr);
    return json::JsonValue::nullValue();
  }
  // 缩略图路径就地计算（不查库）
  for (PhotoRecord& p : result.items) {
    std::string cache = joinPath(config_->thumbsDir(),
                                 std::to_string(320) + "/" + std::to_string(p.id) + ".jpg");
    if (fileExists(cache)) {
      p.thumbState = "ready";
      p.thumbPath = cache;
    } else {
      p.thumbState = "pending";
      p.thumbPath = "";
    }
  }
  return toQueryResult(result);
}

json::JsonValue QueryEngine::search(const json::JsonValue& params, rpc::RpcError* err) {
  const json::JsonValue& text = params.get("text");
  if (!text.isString() || text.stringRef().empty()) {
    *err = rpc::RpcError(rpc::kInvalidParams, "params.text is required");
    return json::JsonValue::nullValue();
  }
  Filter filter;
  std::string filterErr;
  if (!filterFromJson(params.get("filter"), &filter, &filterErr)) {
    *err = rpc::RpcError(rpc::kInvalidParams, filterErr);
    return json::JsonValue::nullValue();
  }
  Page page;
  pageFromJson(params.get("page"), &page);

  QueryResult result;
  std::string dbErr;
  if (!photos_->search(text.stringRef(), filter, page, &result, &dbErr)) {
    *err = rpc::RpcError(rpc::kDbError, dbErr);
    return json::JsonValue::nullValue();
  }
  for (PhotoRecord& p : result.items) {
    std::string cache = joinPath(config_->thumbsDir(),
                                 std::to_string(320) + "/" + std::to_string(p.id) + ".jpg");
    if (fileExists(cache)) {
      p.thumbState = "ready";
      p.thumbPath = cache;
    } else {
      p.thumbState = "pending";
      p.thumbPath = "";
    }
  }
  return toQueryResult(result);
}

json::JsonValue QueryEngine::getFacets(const json::JsonValue& params, rpc::RpcError* err) {
  Filter filter;
  std::string filterErr;
  if (!filterFromJson(params.get("filter"), &filter, &filterErr)) {
    *err = rpc::RpcError(rpc::kInvalidParams, filterErr);
    return json::JsonValue::nullValue();
  }
  json::JsonValue out = json::JsonValue::objectValue();
  std::string dbErr;
  if (!photos_->facets(filter, &out, &dbErr)) {
    *err = rpc::RpcError(rpc::kDbError, dbErr);
    return json::JsonValue::nullValue();
  }
  return out;
}

json::JsonValue QueryEngine::getTimeline(const json::JsonValue& params, rpc::RpcError* err) {
  Filter filter;
  std::string filterErr;
  if (!filterFromJson(params.get("filter"), &filter, &filterErr)) {
    *err = rpc::RpcError(rpc::kInvalidParams, filterErr);
    return json::JsonValue::nullValue();
  }
  std::vector<MonthBucket> buckets;
  std::string dbErr;
  if (!photos_->timeline(filter, &buckets, &dbErr)) {
    *err = rpc::RpcError(rpc::kDbError, dbErr);
    return json::JsonValue::nullValue();
  }
  json::JsonValue out = json::JsonValue::arrayValue();
  for (const MonthBucket& b : buckets) {
    json::JsonValue j = json::JsonValue::objectValue();
    j.setItem("month", b.month);
    j.setItem("count", b.count);
    j.setItem("firstPhotoId", b.firstPhotoId);
    out.addItem(j);
  }
  return out;
}

json::JsonValue QueryEngine::getStats(rpc::RpcError* err) {
  json::JsonValue out = json::JsonValue::objectValue();
  std::string dbErr;
  if (!photos_->stats(&out, &dbErr)) {
    *err = rpc::RpcError(rpc::kDbError, dbErr);
    return json::JsonValue::nullValue();
  }
  return out;
}

json::JsonValue QueryEngine::getFolderTree(rpc::RpcError* err) {
  std::string dbErr;
  std::vector<RootRecord> roots = photos_->listRoots(&dbErr);
  std::vector<db::FolderInfo> folders = photos_->listFolders(&dbErr);
  // 每个根挂它自己的子树
  json::JsonValue out = json::JsonValue::arrayValue();
  for (const RootRecord& root : roots) {
    FolderNode node;
    node.id = 0;
    node.rootId = root.id;
    node.path = root.path;
    node.name = root.label.empty() ? baseName(root.path) : root.label;
    for (const db::FolderInfo& f : folders) {
      if (f.rootId != root.id) continue;
      FolderNode child;
      child.id = f.id;
      child.rootId = f.rootId;
      child.path = f.relPath;
      child.name = f.name;
      child.count = f.count;
      child.missing = f.missing;
      node.children.push_back(child);
      node.count += f.count;
    }
    out.addItem(folderNodeToJson(node));
  }
  return out;
}

json::JsonValue QueryEngine::getPhotoDetail(const json::JsonValue& params, rpc::RpcError* err) {
  const json::JsonValue& idValue = params.get("id");
  if (!idValue.isNumber()) {
    *err = rpc::RpcError(rpc::kInvalidParams, "params.id is required");
    return json::JsonValue::nullValue();
  }
  int64_t id = idValue.asInt();
  PhotoRecord p;
  std::string dbErr;
  if (!photos_->getById(id, &p, &dbErr)) {
    *err = rpc::RpcError(rpc::kFileNotFound, "photo not found: " + std::to_string(id));
    return json::JsonValue::nullValue();
  }
  std::string cache = joinPath(config_->thumbsDir(), "320/" + std::to_string(id) + ".jpg");
  if (fileExists(cache)) {
    p.thumbState = "ready";
    p.thumbPath = cache;
  } else {
    p.thumbState = "pending";
    p.thumbPath = "";
  }
  return toPhotoDetail(p);
}

json::JsonValue QueryEngine::updatePhotos(const json::JsonValue& params, rpc::RpcError* err) {
  std::vector<int64_t> ids = parseIdList(params.get("ids"));
  if (ids.empty()) {
    *err = rpc::RpcError(rpc::kInvalidParams, "params.ids must not be empty");
    return json::JsonValue::nullValue();
  }
  const json::JsonValue& patch = params.get("patch");
  if (!patch.isObject()) {
    *err = rpc::RpcError(rpc::kInvalidParams, "params.patch must be an object");
    return json::JsonValue::nullValue();
  }
  // 需要写 sidecar 的字段交给上层（methods.cpp）处理；此处只落索引层
  int updated = 0;
  std::string dbErr;
  if (!photos_->updateFlags(ids, patch, &updated, &dbErr)) {
    *err = rpc::RpcError(rpc::kDbError, dbErr);
    return json::JsonValue::nullValue();
  }
  json::JsonValue out = json::JsonValue::objectValue();
  out.setItem("updated", updated);
  return out;
}

json::JsonValue QueryEngine::removePhotos(const json::JsonValue& params, rpc::RpcError* err) {
  std::vector<int64_t> ids = parseIdList(params.get("ids"));
  if (ids.empty()) {
    *err = rpc::RpcError(rpc::kInvalidParams, "params.ids must not be empty");
    return json::JsonValue::nullValue();
  }
  int removed = 0;
  std::string dbErr;
  if (!photos_->softRemove(ids, &removed, &dbErr)) {
    *err = rpc::RpcError(rpc::kDbError, dbErr);
    return json::JsonValue::nullValue();
  }
  logInfo("library", "soft-removed " + std::to_string(removed) +
                         " photos from index (originals untouched)");
  json::JsonValue out = json::JsonValue::objectValue();
  out.setItem("removed", removed);
  return out;
}

json::JsonValue QueryEngine::restorePhotos(const json::JsonValue& params, rpc::RpcError* err) {
  std::vector<int64_t> ids = parseIdList(params.get("ids"));
  if (ids.empty()) {
    *err = rpc::RpcError(rpc::kInvalidParams, "params.ids must not be empty");
    return json::JsonValue::nullValue();
  }
  int restored = 0;
  std::string dbErr;
  if (!photos_->restore(ids, &restored, &dbErr)) {
    *err = rpc::RpcError(rpc::kDbError, dbErr);
    return json::JsonValue::nullValue();
  }
  json::JsonValue out = json::JsonValue::objectValue();
  out.setItem("restored", restored);
  return out;
}

}  // namespace library
}  // namespace lumina
