// 查询引擎：Filter/Sort/Page → 预编译 SQL；facets、时间轴、统计、文件夹树
#ifndef LUMINA_LIBRARY_QUERY_ENGINE_H_
#define LUMINA_LIBRARY_QUERY_ENGINE_H_

#include <string>

#include "core/config.h"
#include "core/models.h"
#include "db/database.h"
#include "db/photo_dao.h"
#include "json/json_value.h"
#include "rpc/dispatcher.h"

namespace lumina {
namespace library {

class QueryEngine {
 public:
  QueryEngine(core::Config* config, db::Database* db, db::PhotoDao* photos)
      : config_(config), db_(db), photos_(photos) {}

  json::JsonValue queryPhotos(const json::JsonValue& params, rpc::RpcError* err);
  json::JsonValue search(const json::JsonValue& params, rpc::RpcError* err);
  json::JsonValue getFacets(const json::JsonValue& params, rpc::RpcError* err);
  json::JsonValue getTimeline(const json::JsonValue& params, rpc::RpcError* err);
  json::JsonValue getStats(rpc::RpcError* err);
  json::JsonValue getFolderTree(rpc::RpcError* err);
  json::JsonValue getPhotoDetail(const json::JsonValue& params, rpc::RpcError* err);
  json::JsonValue updatePhotos(const json::JsonValue& params, rpc::RpcError* err);
  json::JsonValue removePhotos(const json::JsonValue& params, rpc::RpcError* err);
  json::JsonValue restorePhotos(const json::JsonValue& params, rpc::RpcError* err);

 private:
  core::Config* config_;
  db::Database* db_;
  db::PhotoDao* photos_;
};

}  // namespace library
}  // namespace lumina

#endif  // LUMINA_LIBRARY_QUERY_ENGINE_H_
