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
  // 仅用于 500ms 防抖节流（tick 的触发条件），**不参与任何因果判断**。
  int64_t lastChangeMs = 0;
  // 单调递增的变更序号：每次置 dirty 时 +1。序号是因果令牌，
  // 时间戳不是——system_clock 可被回拨，且毫秒粒度会让同毫秒内的
  // 「写盘开始」与「新变更」比较相等，从而误清 dirty（丢最后一次拖动）。
  int64_t changeSeq = 0;
  // 最近一次成功落盘所覆盖到的变更序号（= 落盘开始瞬间的 changeSeq 快照）。
  // tick()/end() 以 changeSeq <= flushedSeq 判定「无未落盘变更」。
  int64_t flushedSeq = 0;
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
  // 启动周期落盘线程（500ms 巡检一次，幂等）。必须在 begin() 之前调用，
  // 由 main 在装配完成后启动；未调用时行为与旧版一致（仅靠 end()/shutdown() 落盘）。
  void startFlushThread();
  void shutdown();

 private:
  // 收尾补刷的结果：把「真的写盘失败」和「补刷没追平」分开。
  // 两者的对用户含义不同——前者是 IO/冲突错误（可重试），后者是写盘期间仍有新变更
  // 到达（盘上不是最后值），绝不能混成一个 bool 让调用方误报成功。
  enum class FlushOutcome {
    kClean = 0,     // 收尾时 changeSeq <= flushedSeq：盘上就是最后值
    kWriteFailed,   // flushSession 失败，*err 为落盘错误原因
    kPending,       // 补刷轮次耗尽仍有未落盘变更；*err 不被改成落盘错误
  };

  std::shared_ptr<SessionState> findSession(int64_t sessionId);
  bool flushSession(const std::shared_ptr<SessionState>& session, std::string* err);
  // 有界补刷直到没有未落盘变更（flushSession 写盘期间仍可能有 setParams 到达）。
  // 返回 kClean/kWriteFailed/kPending 三态，语义见 FlushOutcome。
  FlushOutcome flushUntilClean(const std::shared_ptr<SessionState>& session, std::string* err,
                               int maxRounds = 3);

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
  // M2 T2.3 落盘线程：stopFlush_ 置位后线程在处理完当前巡检即退出，shutdown() 负责 join。
  std::atomic<bool> stopFlush_;
  std::thread flushThread_;
};

}  // namespace edit
}  // namespace lumina

#endif  // LUMINA_EDIT_EDIT_SESSION_H_
