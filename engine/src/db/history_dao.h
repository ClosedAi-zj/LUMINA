// M2 T2.4 · history 落盘 DAO（架构 §3.3.3）
//
// **history 是可丢弃派生日志，XMP sidecar 仍是唯一参数真相源。** 四条铁律：
//  1) 参数写入路径唯一（XmpService::saveParams）；本 DAO 不碰 sidecar。
//  2) 单 op 一条事务（BEGIN IMMEDIATE → 丢弃 redo 分支 + INSERT + 推进 current_seq →
//     COMMIT，H4 强杀无半写）；折叠（checkpoint）同样单事务。
//  3) 表缺失（迁移 004 未执行）时所有读写显式失败并写 err，由调用方 logWarn——
//     内存 undo/redo 不受影响，绝不静默。
//  4) base_hash 用 T2.3 同款参数指纹（canonicalJson + MD5，见 DevelopSettingsDao::hashParams）。
//     架构写的 sha1 引擎无实现；该字段只做一致性校验、不参与安全，语义等价（偏离已注明）。
#ifndef LUMINA_DB_HISTORY_DAO_H_
#define LUMINA_DB_HISTORY_DAO_H_

#include <cstdint>
#include <string>
#include <vector>

#include "db/database.h"
#include "json/json_value.h"

namespace lumina {
namespace db {

// history_state 行（photo_id 为主键；exists=false 表示无行）
struct HistoryStateRow {
  bool exists = false;
  int64_t photoId = 0;
  int64_t currentSeq = 0;  // 已重放到的 op 序号（undo/redo 游标，单图内单调推进）
  std::string baseJson;    // 最近 checkpoint 的完整参数
  std::string baseHash;    // hashOf(baseJson)
  int64_t updatedAt = 0;
};

// history_entries 行（一行业 = 一次 commitHistory 的 diff op）
struct HistoryEntryRow {
  int64_t seq = 0;
  std::string opJson;
  std::string label;  // UI 显示名（"exposure: 0.1 → 0.15"）
  int64_t createdAt = 0;
};

class HistoryDao {
 public:
  explicit HistoryDao(Database* db) : db_(db) {}

  // 折叠周期：每 50 步把当前参数折叠为 checkpoint 并删更早 diff（架构 §3.3.3）
  static constexpr int kFoldEvery = 50;
  // 单图历史容量：500 步（内存栈同口径裁剪，见 edit/history.h kHistoryMaxSteps）
  static constexpr int kMaxSteps = 500;

  // history_entries 表是否存在（迁移 004 是否已执行）。缺失时读写全部显式失败。
  bool available(std::string* err) const;

  // 读 history_state 行。found（out.exists）=false 表示无行（不是错误）。
  bool state(int64_t photoId, HistoryStateRow* out, std::string* err);

  // 无行时写入 base（current_seq=0，base=params@打开时刻）；已有行 no-op（幂等）。
  // begin 时调用：为后续 append 提供游标锚点。
  bool initBase(int64_t photoId, const json::JsonValue& baseParams, std::string* err);

  // 单 op 落盘（一条事务）：丢弃 seq>current 的 redo 分支（与 HistoryStack.push 清
  // redo 分支同语义）→ INSERT seq=current+1 → 推进 current_seq。返回分配的 seq。
  bool append(int64_t photoId, const json::JsonValue& op, const std::string& label,
              int64_t sessionId, int64_t* outSeq, std::string* err);

  // undo/redo 移动游标（不增删 diff；跨会话 redo 分支的行保留但恢复时被跳过）。
  bool updateSeq(int64_t photoId, int64_t seq, std::string* err);

  // 折叠（一条事务）：upsert state(base=params@baseSeq) + DELETE seq<=baseSeq 的 diff。
  // 折叠点之前的 undo 深度让位给容量（可丢弃派生口径）；穿越折叠点的 undo 之后
  // 重启会因指纹不一致显式重置（见 EditSessionManager::begin）。
  bool checkpoint(int64_t photoId, int64_t baseSeq, const json::JsonValue& baseParams,
                  std::string* err);

  // 全部 diff 行，seq 升序（含 redo 分支；恢复方按 current_seq 过滤）
  bool list(int64_t photoId, std::vector<HistoryEntryRow>* out, std::string* err);

  // 清空某图全部历史（指纹不一致重置 / 照片删除时调用）
  bool clear(int64_t photoId, std::string* err);

  // 参数指纹（canonical + MD5，与 develop_settings 的 params_hash 同口径）
  static std::string hashOf(const json::JsonValue& params);

 private:
  Database* db_;
};

}  // namespace db
}  // namespace lumina

#endif  // LUMINA_DB_HISTORY_DAO_H_
