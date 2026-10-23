// FTS5 索引维护实现
#include "db/fts_indexer.h"

#include <string>
#include <vector>

#include "core/logger.h"

namespace lumina {
namespace db {

bool FtsIndexer::rebuild(std::string* err) {
  std::string sql = "INSERT INTO photos_fts(photos_fts) VALUES('rebuild');";
  if (!db_->exec(sql, err)) {
    logError("db", "fts rebuild failed: " + (err ? *err : std::string()));
    return false;
  }
  logInfo("db", "fts index rebuilt");
  return true;
}

bool FtsIndexer::buildMatchQuery(const std::string& text, std::string* out) {
  std::string match;
  std::vector<std::string> tokens;
  std::string cur;
  for (char c : text) {
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
      if (!cur.empty()) tokens.push_back(cur);
      cur.clear();
    } else {
      cur.push_back(c);
    }
  }
  if (!cur.empty()) tokens.push_back(cur);
  for (size_t i = 0; i < tokens.size(); ++i) {
    if (i) match += " ";
    match += "\"";
    for (char c : tokens[i]) {
      if (c == '"') match += "\"\"";
      else match.push_back(c);
    }
    match += "\"";
  }
  if (match.empty()) match = "\"\"";
  *out = match;
  return true;
}

}  // namespace db
}  // namespace lumina
