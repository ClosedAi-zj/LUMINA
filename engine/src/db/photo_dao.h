// photos 表 CRUD、多维筛选查询、分页、facets、时间轴聚合、软删除/恢复
#ifndef LUMINA_DB_PHOTO_DAO_H_
#define LUMINA_DB_PHOTO_DAO_H_

#include <string>
#include <vector>

#include "core/models.h"
#include "db/database.h"
#include "json/json_value.h"

namespace lumina {
namespace db {

struct FolderInfo {
  int64_t id = 0;
  int64_t rootId = 0;
  std::string relPath;
  std::string name;
  bool missing = false;
  int64_t count = 0;
};

class PhotoDao {
 public:
  explicit PhotoDao(Database* db) : db_(db) {}

  Database* database() const { return db_; }

  // ---- 根节点 / 文件夹 ----
  int64_t ensureRoot(const std::string& path, const std::string& label, std::string* err);
  int64_t ensureFolder(int64_t rootId, const std::string& relPath, const std::string& name,
                       std::string* err);
  std::vector<RootRecord> listRoots(std::string* err);
  std::vector<FolderInfo> listFolders(std::string* err);

  // ---- 照片 ----
  bool upsert(const PhotoRecord& p, int64_t* outId, std::string* err);
  bool getById(int64_t id, PhotoRecord* out, std::string* err);
  bool getByPath(const std::string& path, PhotoRecord* out, std::string* err);
  bool updateAll(const PhotoRecord& p, std::string* err);

  bool query(const Filter& f, const Sort& s, const Page& pg, QueryResult* out, std::string* err);
  bool search(const std::string& text, const Filter& f, const Page& pg, QueryResult* out,
              std::string* err);
  int64_t count(const Filter& f, std::string* err);
  bool listIds(const Filter& f, std::vector<int64_t>* out, std::string* err);

  bool updateFlags(const std::vector<int64_t>& ids, const json::JsonValue& patch, int* updated,
                   std::string* err);
  bool softRemove(const std::vector<int64_t>& ids, int* removed, std::string* err);
  bool restore(const std::vector<int64_t>& ids, int* restored, std::string* err);
  bool setMissing(int64_t id, bool missing, std::string* err);
  bool setHealth(int64_t id, const std::string& health, std::string* err);
  bool setMd5(int64_t id, const std::string& md5, std::string* err);
  bool setSidecar(int64_t id, const std::string& sidecarPath, int64_t mtime, bool hasEdits,
                  std::string* err);
  bool setGps(int64_t id, double lat, double lng, double alt, const std::string& locationName,
              std::string* err);
  bool setSeqId(int64_t id, int64_t seqId, std::string* err);
  bool setPairKind(int64_t id, const std::string& pairKind, std::string* err);
  bool setBackup(int64_t id, const std::string& backupPath, int64_t verifiedAt, std::string* err);
  bool deletePermanently(const std::vector<int64_t>& ids, std::string* err);

  // ---- 聚合 ----
  bool facets(const Filter& f, json::JsonValue* out, std::string* err);
  bool timeline(const Filter& f, std::vector<MonthBucket>* out, std::string* err);
  bool stats(json::JsonValue* out, std::string* err);
  bool byFormat(json::JsonValue* out, std::string* err);

 private:
  Database* db_;
};

}  // namespace db
}  // namespace lumina

#endif  // LUMINA_DB_PHOTO_DAO_H_
