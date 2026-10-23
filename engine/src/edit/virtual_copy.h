// 虚拟副本 / 快照：非破坏性第二版本与命名状态点（映射表 §5 lumina:）
#ifndef LUMINA_EDIT_VIRTUAL_COPY_H_
#define LUMINA_EDIT_VIRTUAL_COPY_H_

#include <string>

#include "core/config.h"
#include "db/database.h"
#include "db/photo_dao.h"
#include "json/json_value.h"
#include "rpc/dispatcher.h"
#include "xmp/xmp_service.h"

namespace lumina {
namespace edit {

class VirtualCopyService {
 public:
  VirtualCopyService(core::Config* config, db::Database* db, db::PhotoDao* photos,
                     xmp::XmpService* xmp);

  json::JsonValue create(int64_t photoId, const std::string& name, rpc::RpcError* err);
  json::JsonValue list(int64_t photoId, rpc::RpcError* err);

 private:
  core::Config* config_;
  db::Database* db_;
  db::PhotoDao* photos_;
  xmp::XmpService* xmp_;
};

}  // namespace edit
}  // namespace lumina

#endif  // LUMINA_EDIT_VIRTUAL_COPY_H_
