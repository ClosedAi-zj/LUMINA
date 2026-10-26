// 编辑会话：内存会话 + 历史栈 + 500ms 防抖落盘 sidecar
#ifndef LUMINA_EDIT_EDIT_SESSION_H_
#define LUMINA_EDIT_EDIT_SESSION_H_

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "core/thread_pool.h"
#include "db/develop_settings_dao.h"
#include "db/history_dao.h"
#include "db/photo_dao.h"
#include "edit/history.h"
#include "json/json_value.h"
#include "rpc/dispatcher.h"
#include "xmp/xmp_service.h"

namespace lumina {
namespace edit {

struct SessionState {
  int64_t sessionId = 0;
  int64_t photoId = 0;
  json::JsonValue params;
  HistoryStack history;
  int64_t lastChangeMs = 0;
  bool dirty = false;
};

class EditSessionManager {
 public:
  EditSessionManager(xmp::XmpService* xmp, db::PhotoDao* photos);
  ~EditSessionManager();

  json::JsonValue begin(int64_t photoId, rpc::RpcError* err);
  json::JsonValue setParams(int64_t sessionId, const json::JsonValue& patch, bool commitHistory,
                            rpc::RpcError* err);
  json::JsonValue undo(int64_t sessionId, rpc::RpcError* err);
  json::JsonValue redo(int64_t sessionId, rpc::RpcError* err);
  json::JsonValue end(int64_t sessionId, bool flush, rpc::RpcError* err);
  json::JsonValue snapshots(int64_t photoId, rpc::RpcError* err);
  int64_t createSnapshot(int64_t photoId, const std::string& name, rpc::RpcError* err);
  json::JsonValue restoreSnapshot(int64_t photoId, int64_t snapshotId, rpc::RpcError* err);

  // 防抖落盘巡检（由主循环周期性调用）
  void tick();
  void shutdown();

 private:
  std::shared_ptr<SessionState> findSession(int64_t sessionId);
  bool flushSession(const std::shared_ptr<SessionState>& session, std::string* err);

  xmp::XmpService* xmp_;
  db::PhotoDao* photos_;
  // M2 T2.3：参数派生缓存。构造时由 photos->database() 现取，避免改动调用方签名
  // （main.cpp / test_rpc_smoke.cpp 都按两参构造）。
  db::DevelopSettingsDao developSettings_;
  // M2 T2.4：编辑历史持久化（可丢弃派生；sidecar 仍是参数唯一真相源）
  db::HistoryDao history_;
  std::mutex mutex_;
  std::map<int64_t, std::shared_ptr<SessionState>> sessions_;
  int64_t nextSessionId_ = 1;
  std::atomic<bool> running_;
  std::thread flushThread_;
};

}  // namespace edit
}  // namespace lumina

#endif  // LUMINA_EDIT_EDIT_SESSION_H_
