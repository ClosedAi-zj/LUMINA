// 全部服务实例的聚合容器 + 方法注册入口（main.cpp 组装）
#ifndef LUMINA_RPC_METHODS_H_
#define LUMINA_RPC_METHODS_H_

#include <functional>
#include <string>

#include "core/config.h"
#include "core/job_manager.h"
#include "db/album_dao.h"
#include "db/database.h"
#include "db/fts_indexer.h"
#include "db/health_dao.h"
#include "db/image_dao.h"
#include "db/photo_dao.h"
#include "edit/edit_session.h"
#include "edit/virtual_copy.h"
#include "health/backup.h"
#include "health/scanner.h"
#include "health/watcher.h"
#include "import/importer.h"
#include "json/json_value.h"
#include "library/query_engine.h"
#include "library/smart_collections.h"
#include "mock/generator.h"
#include "raw/raw_decoder.h"
#include "rpc/dispatcher.h"
#include "rpc/rpc_server.h"
#include "thumb/thumbnail_service.h"
#include "xmp/migrator.h"
#include "xmp/xmp_service.h"

namespace lumina {
namespace rpc {

// 引擎全部服务句柄（由 main.cpp 构造后注入）
struct EngineServices {
  core::Config* config = nullptr;
  db::Database* db = nullptr;
  db::PhotoDao* photos = nullptr;
  db::ImageDao* images = nullptr;  // M2 T2.2b：四级表身份层（relocate 入口 + 导入建档）
  db::AlbumDao* albums = nullptr;
  db::HealthDao* health = nullptr;
  db::FtsIndexer* fts = nullptr;
  library::QueryEngine* query = nullptr;
  thumb::ThumbnailService* thumbs = nullptr;
  raw::RawDecoder* decoder = nullptr;
  xmp::XmpService* xmp = nullptr;
  xmp::XmpMigrator* migrator = nullptr;
  edit::EditSessionManager* sessions = nullptr;
  edit::VirtualCopyService* vcopy = nullptr;
  import::Importer* importer = nullptr;
  health::HealthScanner* healthScanner = nullptr;
  health::BackupService* backup = nullptr;
  health::HealthWatcher* watcher = nullptr;
  mock::MockGenerator* mock = nullptr;
  JobManager* jobs = nullptr;
  RpcServer* server = nullptr;
  std::function<void()> requestShutdown;
  std::function<void()> runOneTick;
};

// 注册全部已实现方法；未实现领域返回明确错误（绝不静默失败）
void registerMethods(Dispatcher* dispatcher, EngineServices* s);

// 预置数据（内置预设等）
void seedPresetData(EngineServices* s);

}  // namespace rpc
}  // namespace lumina

#endif  // LUMINA_RPC_METHODS_H_
