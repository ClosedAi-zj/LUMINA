// 健康异常清单：记录、查询、处理（任何异常不得被吞掉）
#ifndef LUMINA_DB_HEALTH_DAO_H_
#define LUMINA_DB_HEALTH_DAO_H_

#include <string>
#include <vector>

#include "core/models.h"
#include "db/database.h"

namespace lumina {
namespace db {

class HealthDao {
 public:
  explicit HealthDao(Database* db) : db_(db) {}

  // 记录（或更新）一条异常；同一 photoId+kind+open 状态只保留一条
  int64_t recordIssue(int64_t photoId, const std::string& kind, const std::string& path,
                      const std::string& lastKnown, std::string* err);
  bool listIssues(const std::string& kind, int page, int pageSize, std::vector<HealthIssue>* out,
                  int64_t* total, std::string* err);
  bool resolveIssue(int64_t issueId, const std::string& status, std::string* err);
  bool closeIssuesFor(int64_t photoId, std::string* err);
  bool counts(HealthCounts* out, std::string* err);
  bool setPhotoHealth(int64_t photoId, const std::string& health, std::string* err);

 private:
  Database* db_;
};

}  // namespace db
}  // namespace lumina

#endif  // LUMINA_DB_HEALTH_DAO_H_
