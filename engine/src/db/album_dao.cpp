// 相册 / 精选集 / 画册 DAO 实现
#include "db/album_dao.h"

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

bool AlbumDao::list(const std::string& kind, std::vector<AlbumRecord>* out, std::string* err) {
  std::string sql = "SELECT a.id, a.name, a.kind, a.cover_photo_id, a.published_token, "
                    "a.published_at, a.expires_at, (SELECT COUNT(*) FROM album_items i WHERE "
                    "i.album_id=a.id) c FROM albums a";
  std::vector<Bind> binds;
  if (!kind.empty()) {
    sql += " WHERE a.kind=?";
    binds.push_back(Bind{Bind::kText, 0, kind});
  }
  sql += " ORDER BY a.id;";
  auto stmt = db_->prepareCached("albumsList", sql, err);
  if (!stmt || !stmt->valid()) return false;
  bindAll(stmt.get(), binds, err);
  while (stmt->step(err)) {
    AlbumRecord a;
    a.id = stmt->columnInt64(0);
    a.name = stmt->columnText(1);
    a.kind = stmt->columnText(2);
    a.hasCover = !stmt->columnIsNull(3);
    a.coverPhotoId = stmt->columnInt64(3);
    a.publishedToken = stmt->columnText(4);
    a.hasPublishedAt = !stmt->columnIsNull(5);
    a.publishedAt = stmt->columnInt64(5);
    a.hasExpiresAt = !stmt->columnIsNull(6);
    a.expiresAt = stmt->columnInt64(6);
    a.itemCount = stmt->columnInt64(7);
    out->push_back(a);
  }
  stmt->reset();
  return true;
}

bool AlbumDao::getById(int64_t id, AlbumRecord* out, std::string* err) {
  std::string sql = "SELECT a.id, a.name, a.kind, a.cover_photo_id, a.published_token, "
                    "a.published_at, a.expires_at, (SELECT COUNT(*) FROM album_items i WHERE "
                    "i.album_id=a.id) c FROM albums a WHERE a.id=?;";
  auto stmt = db_->prepareCached("albumGetById", sql, err);
  if (!stmt || !stmt->valid()) return false;
  bindAll(stmt.get(), std::vector<Bind>{Bind{Bind::kInt64, id, std::string()}}, err);
  if (!stmt->step(err)) return false;
  out->id = stmt->columnInt64(0);
  out->name = stmt->columnText(1);
  out->kind = stmt->columnText(2);
  out->hasCover = !stmt->columnIsNull(3);
  out->coverPhotoId = stmt->columnInt64(3);
  out->publishedToken = stmt->columnText(4);
  out->hasPublishedAt = !stmt->columnIsNull(5);
  out->publishedAt = stmt->columnInt64(5);
  out->hasExpiresAt = !stmt->columnIsNull(6);
  out->expiresAt = stmt->columnInt64(6);
  out->itemCount = stmt->columnInt64(7);
  stmt->reset();
  return true;
}

int64_t AlbumDao::create(const std::string& name, const std::string& kind, std::string* err) {
  std::string sql = "INSERT INTO albums(name,kind,created_at) VALUES(?,?,?);";
  auto stmt = db_->prepareCached("albumCreate", sql, err);
  if (!stmt || !stmt->valid()) return 0;
  bindAll(stmt.get(), std::vector<Bind>{Bind{Bind::kText, 0, name}, Bind{Bind::kText, 0, kind},
                                        Bind{Bind::kInt64, nowMs(), std::string()}},
          err);
  if (!stmt->step(err)) return 0;
  int64_t id = db_->lastInsertId();
  stmt->reset();
  return id;
}

bool AlbumDao::rename(int64_t id, const std::string& name, std::string* err) {
  auto stmt = db_->prepareCached("albumRename", "UPDATE albums SET name=? WHERE id=?;", err);
  if (!stmt || !stmt->valid()) return false;
  bindAll(stmt.get(), std::vector<Bind>{Bind{Bind::kText, 0, name}, Bind{Bind::kInt64, id, std::string()}}, err);
  if (!stmt->step(err)) return false;
  stmt->reset();
  return true;
}

bool AlbumDao::remove(int64_t id, std::string* err) {
  {
    auto stmt = db_->prepareCached("albumItemsRemove", "DELETE FROM album_items WHERE album_id=?;", err);
    if (!stmt || !stmt->valid()) return false;
    bindAll(stmt.get(), std::vector<Bind>{Bind{Bind::kInt64, id, std::string()}}, err);
    if (!stmt->step(err)) return false;
    stmt->reset();
  }
  auto stmt = db_->prepareCached("albumRemove", "DELETE FROM albums WHERE id=?;", err);
  if (!stmt || !stmt->valid()) return false;
  bindAll(stmt.get(), std::vector<Bind>{Bind{Bind::kInt64, id, std::string()}}, err);
  if (!stmt->step(err)) return false;
  stmt->reset();
  return true;
}

bool AlbumDao::addPhotos(int64_t albumId, const std::vector<int64_t>& ids, int* added,
                         std::string* err) {
  *added = 0;
  if (ids.empty()) return true;
  auto maxStmt = db_->prepareCached("albumMaxPos",
                                    "SELECT COALESCE(MAX(position),0) FROM album_items WHERE "
                                    "album_id=?;",
                                    err);
  if (!maxStmt || !maxStmt->valid()) return false;
  bindAll(maxStmt.get(), std::vector<Bind>{Bind{Bind::kInt64, albumId, std::string()}}, err);
  int64_t nextPos = 0;
  if (maxStmt->step(err)) nextPos = maxStmt->columnInt64(0);
  maxStmt->reset();

  auto stmt = db_->prepareCached("albumAddPhoto",
                                 "INSERT OR IGNORE INTO album_items(album_id,photo_id,position) "
                                 "VALUES(?,?,?);",
                                 err);
  if (!stmt || !stmt->valid()) return false;
  for (int64_t photoId : ids) {
    bindAll(stmt.get(), std::vector<Bind>{Bind{Bind::kInt64, albumId, std::string()},
                                          Bind{Bind::kInt64, photoId, std::string()},
                                          Bind{Bind::kInt64, ++nextPos, std::string()}},
            err);
    if (!stmt->step(err)) return false;
    if (db_->changes() > 0) ++(*added);
    stmt->reset();
  }
  return true;
}

bool AlbumDao::removePhotos(int64_t albumId, const std::vector<int64_t>& ids, int* removed,
                            std::string* err) {
  *removed = 0;
  if (ids.empty()) return true;
  auto stmt = db_->prepareCached("albumRemovePhoto",
                                 "DELETE FROM album_items WHERE album_id=? AND photo_id=?;", err);
  if (!stmt || !stmt->valid()) return false;
  for (int64_t photoId : ids) {
    bindAll(stmt.get(), std::vector<Bind>{Bind{Bind::kInt64, albumId, std::string()},
                                          Bind{Bind::kInt64, photoId, std::string()}},
            err);
    if (!stmt->step(err)) return false;
    *removed += db_->changes();
    stmt->reset();
  }
  return true;
}

bool AlbumDao::reorder(int64_t albumId, const std::vector<int64_t>& orderedIds, std::string* err) {
  if (!db_->begin(err)) return false;
  auto del = db_->prepareCached("albumReorderDel", "DELETE FROM album_items WHERE album_id=?;", err);
  if (!del || !del->valid()) {
    db_->rollback(err);
    return false;
  }
  bindAll(del.get(), std::vector<Bind>{Bind{Bind::kInt64, albumId, std::string()}}, err);
  if (!del->step(err)) {
    db_->rollback(err);
    return false;
  }
  del->reset();
  int64_t pos = 0;
  auto ins = db_->prepareCached("albumReorderIns",
                                "INSERT INTO album_items(album_id,photo_id,position) VALUES(?,?,?);",
                                err);
  if (!ins || !ins->valid()) {
    db_->rollback(err);
    return false;
  }
  for (int64_t photoId : orderedIds) {
    bindAll(ins.get(), std::vector<Bind>{Bind{Bind::kInt64, albumId, std::string()},
                                         Bind{Bind::kInt64, photoId, std::string()},
                                         Bind{Bind::kInt64, ++pos, std::string()}},
            err);
    if (!ins->step(err)) {
      db_->rollback(err);
      return false;
    }
    ins->reset();
  }
  return db_->commit(err);
}

bool AlbumDao::listItems(int64_t albumId, std::vector<int64_t>* photoIds, std::string* err) {
  auto stmt = db_->prepareCached("albumListItems",
                                 "SELECT photo_id FROM album_items WHERE album_id=? ORDER BY "
                                 "position;",
                                 err);
  if (!stmt || !stmt->valid()) return false;
  bindAll(stmt.get(), std::vector<Bind>{Bind{Bind::kInt64, albumId, std::string()}}, err);
  while (stmt->step(err)) {
    photoIds->push_back(stmt->columnInt64(0));
  }
  stmt->reset();
  return true;
}

bool AlbumDao::setPublished(int64_t albumId, const std::string& token, int64_t publishedAt,
                            int64_t expiresAt, std::string* err) {
  auto stmt = db_->prepareCached("albumPublish",
                                 "UPDATE albums SET published_token=?, published_at=?, expires_at=? "
                                 "WHERE id=?;",
                                 err);
  if (!stmt || !stmt->valid()) return false;
  bindAll(stmt.get(), std::vector<Bind>{Bind{Bind::kText, 0, token},
                                        Bind{Bind::kInt64, publishedAt, std::string()},
                                        Bind{Bind::kInt64, expiresAt, std::string()},
                                        Bind{Bind::kInt64, albumId, std::string()}},
          err);
  if (!stmt->step(err)) return false;
  stmt->reset();
  return true;
}

bool AlbumDao::clearPublished(int64_t albumId, std::string* err) {
  auto stmt = db_->prepareCached("albumUnpublish",
                                 "UPDATE albums SET published_token=NULL, published_at=NULL, "
                                 "expires_at=NULL WHERE id=?;",
                                 err);
  if (!stmt || !stmt->valid()) return false;
  bindAll(stmt.get(), std::vector<Bind>{Bind{Bind::kInt64, albumId, std::string()}}, err);
  if (!stmt->step(err)) return false;
  stmt->reset();
  return true;
}

bool AlbumDao::getByToken(const std::string& token, AlbumRecord* out, std::string* err) {
  auto stmt = db_->prepareCached("albumByToken",
                                 "SELECT id, name, kind, published_token, published_at, expires_at "
                                 "FROM albums WHERE published_token=?;",
                                 err);
  if (!stmt || !stmt->valid()) return false;
  bindAll(stmt.get(), std::vector<Bind>{Bind{Bind::kText, 0, token}}, err);
  if (!stmt->step(err)) return false;
  out->id = stmt->columnInt64(0);
  out->name = stmt->columnText(1);
  out->kind = stmt->columnText(2);
  out->publishedToken = stmt->columnText(3);
  out->hasPublishedAt = !stmt->columnIsNull(4);
  out->publishedAt = stmt->columnInt64(4);
  out->hasExpiresAt = !stmt->columnIsNull(5);
  out->expiresAt = stmt->columnInt64(5);
  stmt->reset();
  return true;
}

bool AlbumDao::listPublished(std::vector<AlbumRecord>* out, std::string* err) {
  auto stmt = db_->prepareCached("albumsPublished",
                                 "SELECT id, name, kind, published_token, published_at, "
                                 "expires_at FROM albums WHERE published_token IS NOT NULL ORDER "
                                 "BY published_at DESC;",
                                 err);
  if (!stmt || !stmt->valid()) return false;
  while (stmt->step(err)) {
    AlbumRecord a;
    a.id = stmt->columnInt64(0);
    a.name = stmt->columnText(1);
    a.kind = stmt->columnText(2);
    a.publishedToken = stmt->columnText(3);
    a.hasPublishedAt = !stmt->columnIsNull(4);
    a.publishedAt = stmt->columnInt64(4);
    a.hasExpiresAt = !stmt->columnIsNull(5);
    a.expiresAt = stmt->columnInt64(5);
    out->push_back(a);
  }
  stmt->reset();
  return true;
}

}  // namespace db
}  // namespace lumina
