// 健康异常清单 DAO 实现
#include "db/health_dao.h"

#include "core/logger.h"
#include "core/str_util.h"

namespace lumina {
namespace db {
namespace {

struct Bind {
  enum Kind { kInt, kInt64, kText } kind = kInt;
  int64_t i = 0;
  std::string s;
};

void bindAll(Stmt* stmt, const std::vector<Bind>& binds, std::string* err) {
  for (size_t idx = 0; idx < binds.size(); ++idx) {
    int pos = static_cast<int>(idx) + 1;
    bool ok = true;
    const Bind& b = binds[idx];
    if (b.kind == Bind::kText) ok = stmt->bindText(pos, b.s);
    else if (b.kind == Bind::kInt64) ok = stmt->bindInt64(pos, b.i);
    else ok = stmt->bindInt(pos, static_cast<int>(b.i));
    if (!ok) {
      if (err) *err = "bind failed";
      return;
    }
  }
}

}  // namespace

int64_t HealthDao::recordIssue(int64_t photoId, const std::string& kind, const std::string& path,
                               const std::string& lastKnown, std::string* err) {
  // 已有同源 open 记录 → 更新检测时间；否则插入
  auto existing = db_->prepareCached(
      "healthFind", "SELECT id FROM health_issues WHERE photo_id=? AND kind=? AND status='open';",
      err);
  if (!existing || !existing->valid()) return 0;
  bindAll(existing.get(), std::vector<Bind>{Bind{Bind::kInt64, photoId, std::string()},
                                            Bind{Bind::kText, 0, kind}},
          err);
  if (existing->step(err)) {
    int64_t id = existing->columnInt64(0);
    existing->reset();
    auto upd = db_->prepareCached(
        "healthTouch", "UPDATE health_issues SET path=?, last_known=?, detected_at=? WHERE id=?;",
        err);
    if (!upd || !upd->valid()) return 0;
    bindAll(upd.get(), std::vector<Bind>{Bind{Bind::kText, 0, path},
                                         Bind{Bind::kText, 0, lastKnown},
                                         Bind{Bind::kInt64, nowMs(), std::string()},
                                         Bind{Bind::kInt64, id, std::string()}},
            err);
    if (!upd->step(err)) return 0;
    upd->reset();
    return id;
  }
  existing->reset();
  auto ins = db_->prepareCached(
      "healthInsert",
      "INSERT INTO health_issues(photo_id,kind,path,last_known,status,detected_at) "
      "VALUES(?,?,?,?,'open',?);",
      err);
  if (!ins || !ins->valid()) return 0;
  bindAll(ins.get(), std::vector<Bind>{Bind{Bind::kInt64, photoId, std::string()},
                                       Bind{Bind::kText, 0, kind},
                                       Bind{Bind::kText, 0, path},
                                       Bind{Bind::kText, 0, lastKnown},
                                       Bind{Bind::kInt64, nowMs(), std::string()}},
          err);
  if (!ins->step(err)) return 0;
  int64_t id = db_->lastInsertId();
  ins->reset();
  logWarn("health", "issue recorded: photo " + std::to_string(photoId) + " kind " + kind + " path " +
                        path);
  return id;
}

bool HealthDao::listIssues(const std::string& kind, int page, int pageSize,
                           std::vector<HealthIssue>* out, int64_t* total, std::string* err) {
  if (pageSize <= 0) pageSize = 50;
  if (page < 0) page = 0;
  std::string where = " WHERE status='open'";
  std::vector<Bind> binds;
  if (!kind.empty()) {
    where += " AND kind=?";
    binds.push_back(Bind{Bind::kText, 0, kind});
  }
  auto cs = db_->prepareCached("healthCount", "SELECT COUNT(*) FROM health_issues" + where + ";",
                               err);
  if (!cs || !cs->valid()) return false;
  bindAll(cs.get(), binds, err);
  if (!cs->step(err)) return false;
  *total = cs->columnInt64(0);
  cs->reset();

  std::string sql = "SELECT id,photo_id,kind,path,last_known,status,detected_at FROM "
                    "health_issues" +
                    where + " ORDER BY detected_at DESC LIMIT ? OFFSET ?;";
  auto stmt = db_->prepareCached("healthList", sql, err);
  if (!stmt || !stmt->valid()) return false;
  // bindAll 每次都从 ?1 开始绑定 → 过滤参数与分页参数必须一次性合并传入。
  // 分两次调用会让第二次的 LIMIT 覆盖掉 kind 的绑定、OFFSET 留空为 NULL，
  // 表现为 count 正确但列表恒空（kind='missing' 时 total=362 / items=[]）。
  std::vector<Bind> listBinds = binds;
  listBinds.push_back(Bind{Bind::kInt, pageSize, std::string()});
  listBinds.push_back(Bind{Bind::kInt, page * pageSize, std::string()});
  bindAll(stmt.get(), listBinds, err);
  while (stmt->step(err)) {
    HealthIssue issue;
    issue.issueId = stmt->columnInt64(0);
    issue.photoId = stmt->columnInt64(1);
    issue.kind = stmt->columnText(2);
    issue.path = stmt->columnText(3);
    issue.lastKnown = stmt->columnText(4);
    issue.status = stmt->columnText(5);
    issue.detectedAt = stmt->columnInt64(6);
    out->push_back(issue);
  }
  stmt->reset();
  return true;
}

bool HealthDao::resolveIssue(int64_t issueId, const std::string& status, std::string* err) {
  auto stmt = db_->prepareCached("healthResolve",
                                 "UPDATE health_issues SET status=?, resolved_at=? WHERE id=?;",
                                 err);
  if (!stmt || !stmt->valid()) return false;
  bindAll(stmt.get(), std::vector<Bind>{Bind{Bind::kText, 0, status},
                                        Bind{Bind::kInt64, nowMs(), std::string()},
                                        Bind{Bind::kInt64, issueId, std::string()}},
          err);
  if (!stmt->step(err)) return false;
  stmt->reset();
  return true;
}

bool HealthDao::closeIssuesFor(int64_t photoId, std::string* err) {
  auto stmt = db_->prepareCached(
      "healthCloseFor",
      "UPDATE health_issues SET status='resolved', resolved_at=? WHERE photo_id=? AND "
      "status='open';",
      err);
  if (!stmt || !stmt->valid()) return false;
  bindAll(stmt.get(), std::vector<Bind>{Bind{Bind::kInt64, nowMs(), std::string()},
                                        Bind{Bind::kInt64, photoId, std::string()}},
          err);
  if (!stmt->step(err)) return false;
  stmt->reset();
  return true;
}

bool HealthDao::counts(HealthCounts* out, std::string* err) {
  auto stmt = db_->prepareCached(
      "healthCounts",
      "SELECT COALESCE(SUM(CASE WHEN health='ok' THEN 1 ELSE 0 END),0), "
      "COALESCE(SUM(CASE WHEN health='corrupt' THEN 1 ELSE 0 END),0), "
      "COALESCE(SUM(CASE WHEN health='unchecked' THEN 1 ELSE 0 END),0), COUNT(*), "
      "COALESCE(SUM(CASE WHEN is_missing=1 THEN 1 ELSE 0 END),0) FROM photos WHERE deleted=0;",
      err);
  if (!stmt || !stmt->valid()) return false;
  if (!stmt->step(err)) return false;
  out->verified = stmt->columnInt64(0);
  out->corrupt = stmt->columnInt64(1);
  out->unchecked = stmt->columnInt64(2);
  out->total = stmt->columnInt64(3);
  out->missing = stmt->columnInt64(4);
  stmt->reset();
  return true;
}

bool HealthDao::setPhotoHealth(int64_t photoId, const std::string& health, std::string* err) {
  auto stmt = db_->prepareCached("healthSet", "UPDATE photos SET health=? WHERE id=?;", err);
  if (!stmt || !stmt->valid()) return false;
  bindAll(stmt.get(), std::vector<Bind>{Bind{Bind::kText, 0, health},
                                        Bind{Bind::kInt64, photoId, std::string()}},
          err);
  if (!stmt->step(err)) return false;
  stmt->reset();
  return true;
}

}  // namespace db
}  // namespace lumina
