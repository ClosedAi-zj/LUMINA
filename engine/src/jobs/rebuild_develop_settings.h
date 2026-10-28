// M2 T2.2b⑥ · 启动回填 job（N36）：develop_settings 派生缓存对齐 sidecar
//
// 真空档背景：迁移 003 只建表不扫盘，rebuildFromSidecar 此前只有测试在调——升级库
// （有已编辑照片但 develop_settings 为空）启动后缓存始终是空的。本模块把它接进
// JobManager：首跑全量、之后增量，低优先级、可取消、有进度、失败不静默。
//
// 首跑判定口径（runRebuildDevelopSettings）：
//   1) available()=false（表缺失/库未开）→ mode="unavailable"，显式 warn，零静默；
//   2) 表存在且 0 行（首跑/升级库）→ mode="full"，全量 rebuildFromSidecar；
//   3) 表非空 → mode="incremental"，增量 backfillFromSidecar（口径见
//      develop_settings_dao.h：无缓存行 或 缓存 mtime < 盘上 mtime）。
#ifndef LUMINA_JOBS_REBUILD_DEVELOP_SETTINGS_H_
#define LUMINA_JOBS_REBUILD_DEVELOP_SETTINGS_H_

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>

#include "json/json_value.h"

namespace lumina {
class JobManager;  // 提交入口需要（start/cancel/进度事件）；注意在 lumina 命名空间，非 core
namespace db {
class Database;  // 仅指针传递，前向声明避免 jobs → db 头依赖
}  // namespace db
namespace xmp {
class XmpService;
}  // namespace xmp

namespace jobs {

// job 结果（event.job.completed 的 result 字段；mode 供调用方与测试断言首跑/增量判定）
struct RebuildJobResult {
  std::string mode = "incremental";  // full | incremental | unavailable
  int scanned = 0;                   // 全量=扫描照片数；增量=实际回填的候选数
  int written = 0;
  int removed = 0;
  int failed = 0;
  bool cancelled = false;
  int64_t durationMs = 0;
};

json::JsonValue rebuildResultToJson(const RebuildJobResult& r);

// 执行函数（可独立测试；测试直接调它，不经 main.cpp 装配）。取消：cancel 置 true
// 后在批间退出，已提交部分保持有效。失败不静默：failed>0 / unavailable 均有 warn。
RebuildJobResult runRebuildDevelopSettings(db::Database* db, xmp::XmpService* xmp,
                                           std::atomic<bool>* cancel,
                                           const std::function<void(int, const std::string&)>&
                                               progress,
                                           std::string* err);

// 向 JobManager 提交 kind="rebuild_develop_settings" 的启动回填 job，立即返回 jobId。
// 提交点约定：迁移 + 服务装配完成后提交（job 在线程池异步执行，不阻塞握手/RPC）；
// 取消由 JobManager::shutdown() 统一置位（应用退出即停，延迟 ≤ 一批）。
int64_t submitRebuildDevelopSettings(JobManager* jobs, db::Database* db,
                                     xmp::XmpService* xmp);

}  // namespace jobs
}  // namespace lumina

#endif  // LUMINA_JOBS_REBUILD_DEVELOP_SETTINGS_H_
