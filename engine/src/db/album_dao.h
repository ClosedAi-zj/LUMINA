// 相册 / 精选集 / 画册：增删项、排序、封面
#ifndef LUMINA_DB_ALBUM_DAO_H_
#define LUMINA_DB_ALBUM_DAO_H_

#include <string>
#include <vector>

#include "core/models.h"
#include "db/database.h"

namespace lumina {
namespace db {

class AlbumDao {
 public:
  explicit AlbumDao(Database* db) : db_(db) {}

  bool list(const std::string& kind, std::vector<AlbumRecord>* out, std::string* err);
  bool getById(int64_t id, AlbumRecord* out, std::string* err);
  int64_t create(const std::string& name, const std::string& kind, std::string* err);
  bool rename(int64_t id, const std::string& name, std::string* err);
  bool remove(int64_t id, std::string* err);
  bool addPhotos(int64_t albumId, const std::vector<int64_t>& ids, int* added, std::string* err);
  bool removePhotos(int64_t albumId, const std::vector<int64_t>& ids, int* removed, std::string* err);
  bool reorder(int64_t albumId, const std::vector<int64_t>& orderedIds, std::string* err);
  bool listItems(int64_t albumId, std::vector<int64_t>* photoIds, std::string* err);
  bool setPublished(int64_t albumId, const std::string& token, int64_t publishedAt, int64_t expiresAt,
                    std::string* err);
  bool clearPublished(int64_t albumId, std::string* err);
  bool getByToken(const std::string& token, AlbumRecord* out, std::string* err);
  bool listPublished(std::vector<AlbumRecord>* out, std::string* err);

 private:
  Database* db_;
};

}  // namespace db
}  // namespace lumina

#endif  // LUMINA_DB_ALBUM_DAO_H_
