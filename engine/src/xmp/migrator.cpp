// XMP 版本迁移器实现
#include "xmp/migrator.h"

#include <memory>
#include <mutex>
#include <vector>

#include "core/logger.h"
#include "core/str_util.h"
#include "rpc/version.h"
#include "xmp/xmp_mapper.h"
#include "xmp/xmp_service.h"

namespace lumina {
namespace xmp {

XmpMigrator::XmpMigrator(core::Config* config, db::PhotoDao* photos, db::Database* db,
                         XmpService* xmp)
    : config_(config), photos_(photos), db_(db), xmp_(xmp) {}

bool XmpMigrator::migrateDocument(XmpDocument* doc, std::string* err) {
  if (!doc || !doc->valid()) {
    if (err) *err = "migrateDocument: invalid doc";
    return false;
  }
  int64_t current = doc->getInt("lumina", "SchemaVersion", 0);
  if (current >= kXmpSchemaVersion) return true;  // 幂等：已迁移/过新均跳过
  if (current > kXmpSchemaVersion) {
    logWarn("xmp", "schema too new (" + std::to_string(current) + "), skipping");
    return true;
  }
  // ---- v1→v2 迁移（架构 §3.5）----
  // ① 构造默认图：把现有 93 叶参数（含 crs: 投影值）拆进 8 全局节点；
  //    已有 NodeGraph（版本字段落后）则保留图只升版本。
  // ② lumina:AdjustmentStack 层映射为节点（在 paramsToGraph 内完成），
  //    映射后写空数组 + Deprecated 标记，避免双真相源。
  // ③ 只增写：XmpDocument 字节保留，未触碰的 crs: 字段逐字节不变。
  // T2.5 v2→v3（架构 §3.2）：同一线路顺带完成——GraphMask 读端对无 combine 的
  // v2 蒙版缺省 replace，写端恒写 combine → 恰好「只增 masks[*].combine」；
  // amount 缺省 100 时不写出 → 其余字段字节稳定；crs: 全量字节保留；
  // lumina:Masks（v1 遗留）不动；不提供 v3→v2 降级（combine=replace 在 v2
  // 读端被忽略即等价，graphFromJson 仍接受 ver≤3）。
  json::JsonValue params;
  std::vector<std::string> warnings;
  XmpMapper::readFromDoc(*doc, &params, &warnings);
  graph::NodeGraph g;
  if (doc->has("lumina", "NodeGraph")) {
    std::string graphErr;
    if (!XmpMapper::readGraph(*doc, &g, &graphErr)) {
      // 图存在但解析失败：不猜测丢弃 → 从 crs:/AdjustmentStack 重建并告警
      logWarn("xmp", "existing NodeGraph unparseable (" + graphErr +
                         "), rebuilding from params");
      g = XmpMapper::paramsToGraph(params);
    }
  } else {
    g = XmpMapper::paramsToGraph(params);
  }
  // 图内 schemaVersion 以文件为准解析（v2 图 = 2），迁移目标恒为当前版本
  g.schemaVersion = kXmpSchemaVersion;
  XmpMapper::writeGraph(g, doc);
  // crs:ProcessVersion 补齐（缺失时；已有则字节保留）
  if (!doc->has("crs", "ProcessVersion")) {
    doc->setText("crs", "ProcessVersion", kMaxProcessVersion);
  }
  // AdjustmentStack 废弃标记（图已承载其内容）
  const json::JsonValue& stack = params.get("adjustmentStack");
  if (stack.isArray() && stack.size() > 0) {
    doc->setText("lumina", "AdjustmentStack", "[]");
    doc->setText("lumina", "AdjustmentStackDeprecated", "true");
  }
  return true;
}

bool XmpMigrator::migrateOne(int64_t photoId, std::string* err) {
  // v1.1.1：与 XmpService::saveParams/setExifGps/clearExifGps 复用同一把 per-photo 写锁。
  // 后台全量迁移（main.cpp 后台线程）与用户实时编辑/定位写入因此串行化，
  // 避免 read-modify-write 竞争丢更新或写坏正在编辑的 sidecar。
  static std::mutex kFallbackMutex;  // xmp_ 为空时的退化串行化（正常构造不会发生）
  std::shared_ptr<std::mutex> photoLock =
      xmp_ ? xmp_->photoWriteMutex(photoId) : std::shared_ptr<std::mutex>();
  std::lock_guard<std::mutex> lock(photoLock ? *photoLock : kFallbackMutex);
  PhotoRecord p;
  if (!photos_->getById(photoId, &p, err)) {
    if (err && err->empty()) *err = "photo not found";
    return false;
  }
  std::string sidecar = p.sidecarPath.empty() ? replaceExtension(p.filePath, "xmp") : p.sidecarPath;
  if (!fileExists(sidecar)) return true;  // 无 sidecar 无需迁移
  std::string bytes;
  if (!readFile(sidecar, &bytes, err)) return false;
  std::string parseErr;
  std::unique_ptr<XmpDocument> doc = XmpDocument::parse(bytes, &parseErr);
  if (!doc) {
    if (err) *err = "xmp parse failed: " + sidecar;
    return false;
  }
  int64_t current = doc->getInt("lumina", "SchemaVersion", 0);
  if (current >= kXmpSchemaVersion) return true;
  if (!migrateDocument(doc.get(), err)) return false;
  std::string output = doc->serialize();
  if (!writeFileAtomic(sidecar, output, err)) return false;
  photos_->setSidecar(photoId, sidecar, fileMtimeMs(sidecar), true, err);
  logInfo("xmp", "migrated sidecar " + sidecar + " from schema " + std::to_string(current) +
                     " to " + std::to_string(kXmpSchemaVersion));
  return true;
}

bool XmpMigrator::migrateAll(int* migrated, std::string* err,
                             const std::atomic<bool>* stop) {
  *migrated = 0;
  std::string sql = "SELECT id FROM photos WHERE deleted=0 AND (sidecar_path IS NOT NULL AND "
                    "sidecar_path<>'') ORDER BY id;";
  auto stmt = db_->prepareCached("migrateAllIds", sql, err);
  if (!stmt || !stmt->valid()) return false;
  std::vector<int64_t> ids;
  while (stmt->step(err)) {
    ids.push_back(stmt->columnInt64(0));
  }
  stmt->reset();
  int count = 0;
  for (int64_t id : ids) {
    // 后台迁移优雅退出：退出流程置位 stop 后，处理完当前张即收尾（单张毫秒级）
    if (stop && stop->load()) {
      logInfo("xmp", "migration pass stopped early at photo " + std::to_string(id) +
                         ", migrated so far=" + std::to_string(count));
      break;
    }
    std::string oneErr;
    if (migrateOne(id, &oneErr)) {
      // 只有真正发生写入才算迁移一次；此处简化：无 sidecar 跳过
      ++count;
    } else {
      logWarn("xmp", "migration skipped for photo " + std::to_string(id) + ": " + oneErr);
    }
  }
  *migrated = count;
  return true;
}

json::JsonValue XmpMigrator::status(rpc::RpcError* err) {
  // ⚠ 本方法必须**只读**：RPC 是同步 dispatch，历史库首次调用时若在这里逐张
  // migrateOne（读-改-写磁盘 + 取 per-photo 写锁），会把整个引擎冻结数分钟
  // —— 期间连 system.ping 都不回，UI 判定引擎死亡。真正的迁移由 main.cpp 的
  // 后台 migrationThread（migrateAll）负责，两者扫描的是同一人群：
  //   migrateAll: SELECT id FROM photos WHERE deleted=0 AND sidecar_path IS NOT NULL
  //               AND sidecar_path<>''
  //   status    : 同一 WHERE 条件（见下方 sql）
  // 因此这里只做版本分布统计，不再触发任何写入。
  json::JsonValue out = json::JsonValue::objectValue();
  out.setItem("current", kXmpSchemaVersion);
  json::JsonValue distribution = json::JsonValue::objectValue();
  distribution.setItem("0", 0);
  distribution.setItem("1", 0);
  distribution.setItem("2", 0);
  distribution.setItem("3", 0);
  int64_t pending = 0;
  std::string sqlErr;
  std::string sql = "SELECT id, sidecar_path FROM photos WHERE deleted=0 AND sidecar_path IS NOT "
                    "NULL AND sidecar_path<>'' ORDER BY id;";
  auto stmt = db_->prepareCached("migrationScan", sql, &sqlErr);
  if (!stmt || !stmt->valid()) {
    *err = rpc::RpcError(rpc::kDbError, sqlErr);
    return json::JsonValue::nullValue();
  }
  int64_t total = 0;
  sqlErr.clear();
  while (stmt->step(&sqlErr)) {
    std::string sidecar = stmt->columnText(1);
    ++total;
    std::string bytes;
    std::string readErr;
    if (!readFile(sidecar, &bytes, &readErr)) {
      // 读不到就无法判定版本：计入 pending（需要后台迁移尝试），但不在这里修
      ++pending;
      continue;
    }
    std::string parseErr;
    std::unique_ptr<XmpDocument> doc = XmpDocument::parse(bytes, &parseErr);
    if (!doc) {
      ++pending;
      continue;
    }
    int64_t version = doc->getInt("lumina", "SchemaVersion", 0);
    if (version < kXmpSchemaVersion) {
      distribution.setItem(std::to_string(version),
                           distribution.get(std::to_string(version)).asInt() + 1);
      ++pending;
    } else {
      // 已达标（含 v3）与过新（>3，1006 schemaTooNew 提示）都计入当前版本桶
      distribution.setItem("3", distribution.get("3").asInt() + 1);
    }
  }
  stmt->reset();
  // 扫描中断不得当成「扫完了」：distribution 会偏小、sidecars 计数错，
  // 用户据此判断「已全部迁移」，属于静默谎报。
  if (!sqlErr.empty()) {
    *err = rpc::RpcError(rpc::kDbError, "xmp.migrationStatus 扫描失败: " + sqlErr);
    return json::JsonValue::nullValue();
  }
  if (pending > 0) {
    logInfo("xmp", "migrationStatus: " + std::to_string(pending) +
                       " 个 sidecar 待迁移，由启动后台迁移线程处理（status 本身只读）");
  }
  out.setItem("distribution", distribution);
  // migrated 保留字段（前端 app/src/api/engine.ts 声明为数字）：status 不再写入，
  // 恒为 0 —— 真实迁移量看后台线程日志，不再用「顺手迁移的张数」冒充状态。
  out.setItem("migrated", static_cast<int64_t>(0));
  out.setItem("pending", pending);
  out.setItem("sidecars", total);
  out.setItem("note", "background migration");
  return out;
}

}  // namespace xmp
}  // namespace lumina
