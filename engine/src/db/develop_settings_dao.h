// M2 T2.3 · develop_settings 派生缓存 DAO（架构 §3.3.2）
//
// **本表是可丢弃派生数据，XMP sidecar 仍是唯一真相源。** 四条铁律：
//  1) 写序：先原子写 sidecar（temp+rename，由 XmpService::saveParams 负责）→ 成功后才
//     upsert 缓存；缓存写失败只 warning，**绝不回滚 sidecar**。
//  2) 参数写入路径唯一（XmpService::saveParams）；本 DAO **不得反向写 sidecar**——
//     它只读 sidecar 产物（rebuildFromSidecar 内部调 XmpService::loadParams）。
//  3) 可重建：删表后 rebuildFromSidecar() 全量重建，查询结果与删表前逐条相等（H5）。
//  4) 可自愈：params_json 解析失败 → 删行 + 从 sidecar 重建 + 显式 warning（H6）；
//     sidecar 比缓存新 → 重读 sidecar 覆盖（debug 日志）；sidecar 缺失 → source='db-stale'
//     + 非空 warning（UI 状态点，零静默）。
#ifndef LUMINA_DB_DEVELOP_SETTINGS_DAO_H_
#define LUMINA_DB_DEVELOP_SETTINGS_DAO_H_

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "db/database.h"
#include "json/json_value.h"

namespace lumina {
namespace xmp {
class XmpService;  // 仅重建时需要（读 sidecar）；前向声明避免 db ←→ xmp 头文件环
}  // namespace xmp

namespace db {

// 派生缓存行（字段与 §3.3.2 的建表语句一一对应）
struct DevelopSettingsRow {
  int64_t photoId = 0;
  std::string paramsJson;
  std::string paramsHash;
  int schemaVersion = 0;
  int64_t sidecarMtime = 0;
  int nodeCount = 0;
  int maskCount = 0;
  bool hasEdits = false;
  int64_t updatedAt = 0;
  std::string source = "xmp";  // xmp | db-stale | rebuilt
};

// 一致性巡检结果（打开/查询照片时由 reconcile 产出）
struct ReconcileResult {
  // ok        = 缓存与 sidecar 一致
  // refreshed = sidecar 比缓存新，已重读覆盖（debug 日志）
  // repaired  = 缓存行损坏，已删行并从 sidecar 重建（warning）
  // stale     = sidecar 缺失，缓存无对应真相源（source='db-stale'，warning）
  // filled    = 缓存原本没有该行，已按 sidecar 补建
  // absent    = sidecar 与缓存都没有（未调整照片）
  // unavailable = develop_settings 表不存在（迁移 003 未执行）——降级，不报错
  std::string state = "absent";
  std::string warning;      // 非空 = 必须显式提示用户（UI 状态点/提示条）
  bool cacheUsable = false; // 调用方可据此决定是否信任缓存里的参数
};

// 全量重建报告（H5：written 应等于有 sidecar 的照片数；removed = 无 sidecar/损坏被清的行）
struct RebuildReport {
  int scanned = 0;
  int written = 0;
  int removed = 0;
  int failed = 0;
  bool cancelled = false;
  std::string detail;
};

class DevelopSettingsDao {
 public:
  explicit DevelopSettingsDao(Database* db) : db_(db) {}

  // develop_settings 表是否存在（迁移 003 是否已执行）。缺失时所有读写降级为 no-op，
  // 由调用方决定提示方式——**不得静默当作「无调整」**。
  bool available(std::string* err) const;

  // 写入/覆盖一行（幂等 upsert）。params 必须是 sidecar 读回的**全量**参数对象。
  bool upsert(int64_t photoId, const json::JsonValue& params, int64_t sidecarMtime,
              const std::string& source, std::string* err);

  // 读取一行。found=false 表示无缓存行（不是错误）。
  bool get(int64_t photoId, DevelopSettingsRow* out, bool* found, std::string* err);

  // 删行（损坏自愈 / 照片删除时调用）
  bool erase(int64_t photoId, std::string* err);

  // 标记「缓存无对应 sidecar」（保留参数内容供 UI 提示与「重建 sidecar」使用）
  bool markStale(int64_t photoId, std::string* err);

  // 一致性巡检（§3.3.2 的四行场景表全部由它实现）。
  // sidecarParams 必须是调用方刚从 sidecar 读回的参数（本函数不回读文件）。
  bool reconcile(int64_t photoId, const json::JsonValue& sidecarParams, int64_t sidecarMtime,
                 bool sidecarExists, ReconcileResult* out, std::string* err);

  // 全量重建：扫 photos，逐张读 sidecar 覆盖缓存。可取消、有进度、失败不静默。
  // H5 的「删表重建后逐条相等」由它保证。
  // yieldBetweenChunks（可空）：每处理完一批（kRebuildChunk 张）后调用一次，供启动
  // 回填 job 做低优先级节流（批间让出 CPU）；普通调用方不传即无节流，行为不变。
  bool rebuildFromSidecar(xmp::XmpService* xmp, RebuildReport* rep, std::atomic<bool>* cancel,
                          const std::function<void(int, const std::string&)>& progress,
                          std::string* err,
                          const std::function<void()>& yieldBetweenChunks = nullptr);

  // 增量回填（M2 T2.2b⑥ N11/N12：rebuildFromSidecar 的增量入口）。
  // 口径（比任务书「has_edits=1」稍宽，理由见下）：
  //   候选 = 「无缓存行且盘上 sidecar 存在」 或 「有缓存行且缓存 sidecar_mtime <
  //   盘上 sidecar mtime」；其余照片只做一次 stat 比对即跳过。
  //   不把候选限定在 has_edits=1：mtime 变新即代表 sidecar 被外部改动（外部编辑
  //   工具给「未调整」照片加调整同理），按 has_edits 过滤会漏回填；mtime 相等或
  //   更旧则无论 has_edits 取值都跳过，常规启动时 0 解析 0 写库。
  //   失配行的两种处理（口径不同，勿混）：
  //   a) sidecar 丢失但缓存行仍在 → 与全量重建同口径删行（防筛选幽灵命中），计入 removed；
  //   b) sidecar 在但 loadParams 解析失败 → **保留旧缓存行** + failed++ + WARN，不删行：
  //      每次启动重试、sidecar 修复后自愈；旧缓存内容仍与上次成功解析一致，删行反而
  //      会把「已调整」照片降级成「未调整」（此处与全量重建的删行口径有意不同）。
  // RebuildReport 语义：scanned = 实际读 sidecar 并回填的候选数（跳过的不计）；
  // written = 候选中成功 upsert 的行数；removed = 删掉的失配行数。可取消（批间
  // 检查，已提交部分保持有效）、有进度、失败不静默。
  bool backfillFromSidecar(xmp::XmpService* xmp, RebuildReport* rep, std::atomic<bool>* cancel,
                           const std::function<void(int, const std::string&)>& progress,
                           std::string* err,
                           const std::function<void()>& yieldBetweenChunks = nullptr);

  // 缓存行计数（启动回填 job 的首跑判定：0 行 → 全量重建）。表不存在时返回 0 行
  // 且不报错——可用性判定走 available()，本方法只管行数。查询失败（库损坏/锁死等
  // step 失败）返回 false 并写明 err，调用方必须显式留痕，不得当作「空表」。
  bool cachedRowCount(int64_t* out, std::string* err);

  // 走部分索引 idx_dev_edited（WHERE has_edits=1）的筛选
  bool queryEdited(std::vector<int64_t>* out, int limit, std::string* err);
  // 走部分索引 idx_dev_masks（WHERE mask_count>0）的筛选
  bool queryWithMasks(std::vector<int64_t>* out, int limit, std::string* err);

  // ---- 纯函数（无 IO，供测试与调用方复用）----
  // 参数是否与默认值有差异（忽略 schemaVersion 这类元数据键）
  static bool computeHasEdits(const json::JsonValue& params);
  // masks 数组长度
  static int countMasks(const json::JsonValue& params);
  // 节点数（NodeGraph 形态的 "nodes" 数组长度；无该键 = 0）
  static int countNodes(const json::JsonValue& params);
  // 规范化 JSON（对象键排序 + 数值统一格式）：同一参数永远得到同一串
  static std::string canonicalJson(const json::JsonValue& v);
  // 参数指纹。注：实现用 MD5（引擎已有 core/md5.h，仓库无 SHA-1 实现）；
  // 该字段只用于变更检测、不参与安全，语义与 §3.3.2 的 sha1 等价。
  static std::string hashParams(const json::JsonValue& params);

 private:
  Database* db_;
};

}  // namespace db
}  // namespace lumina

#endif  // LUMINA_DB_DEVELOP_SETTINGS_DAO_H_
