// FTS5 索引维护：重建、单行同步、匹配串构造（中文可用）
#ifndef LUMINA_DB_FTS_INDEXER_H_
#define LUMINA_DB_FTS_INDEXER_H_

#include <string>

#include "db/database.h"

namespace lumina {
namespace db {

class FtsIndexer {
 public:
  explicit FtsIndexer(Database* db) : db_(db) {}

  // 全量重建（启动兜底）
  bool rebuild(std::string* err);

  // 用户输入 → FTS5 MATCH 串（逐 token 加引号，末 token 前缀匹配）
  static bool buildMatchQuery(const std::string& text, std::string* out);

 private:
  Database* db_;
};

}  // namespace db
}  // namespace lumina

#endif  // LUMINA_DB_FTS_INDEXER_H_
