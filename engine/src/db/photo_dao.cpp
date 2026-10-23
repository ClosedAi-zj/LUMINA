// photos 表 CRUD、多维筛选查询、分页、facets、时间轴聚合、软删除/恢复实现
#include "db/photo_dao.h"

#include <cstdio>
#include <cstring>

#include "core/logger.h"

namespace lumina {
namespace db {
namespace {

// 绑定值载体（按 SQL 占位符顺序绑定）
struct Bind {
  enum Kind { kInt, kInt64, kDouble, kText, kNull } kind = kInt;
  int64_t i = 0;
  double d = 0.0;
  std::string s;
};

void bindAll(Stmt* stmt, const std::vector<Bind>& binds, std::string* err) {
  for (size_t idx = 0; idx < binds.size(); ++idx) {
    int pos = static_cast<int>(idx) + 1;
    const Bind& b = binds[idx];
    bool ok = true;
    switch (b.kind) {
      case Bind::kInt: ok = stmt->bindInt(pos, static_cast<int>(b.i)); break;
      case Bind::kInt64: ok = stmt->bindInt64(pos, b.i); break;
      case Bind::kDouble: ok = stmt->bindDouble(pos, b.d); break;
      case Bind::kText: ok = stmt->bindText(pos, b.s); break;
      case Bind::kNull: ok = stmt->bindNull(pos); break;
    }
    if (!ok) {
      if (err) *err = "bind failed at " + std::to_string(pos);
      return;
    }
  }
}

void quoteText(const std::string& in, std::string* out) {
  *out += "'";
  for (char c : in) {
    if (c == '\'') *out += "''";
    else *out += c;
  }
  *out += "'";
}

}  // namespace

// ---------------- 根节点 / 文件夹 ----------------

int64_t PhotoDao::ensureRoot(const std::string& path, const std::string& label,
                             std::string* err) {
  std::string sql = "INSERT INTO roots(path,label) VALUES(?,?) ON CONFLICT(path) DO UPDATE SET "
                    "label=excluded.label;";
  auto stmt = db_->prepareCached("ensureRoot", sql, err);
  if (!stmt || !stmt->valid()) return 0;
  std::vector<Bind> binds{Bind{Bind::kText, 0, 0, path}, Bind{Bind::kText, 0, 0, label}};
  bindAll(stmt.get(), binds, err);
  if (!stmt->step(err)) return 0;
  sqlite3_int64 id = db_->lastInsertId();
  stmt->reset();
  if (id > 0) return static_cast<int64_t>(id);
  // 冲突时 lastInsertRowid 未必更新，回查一次
  sql = "SELECT id FROM roots WHERE path=?;";
  auto q = db_->prepareCached("ensureRootSelect", sql, err);
  if (!q || !q->valid()) return 0;
  std::vector<Bind> b2{Bind{Bind::kText, 0, 0, path}};
  bindAll(q.get(), b2, err);
  if (!q->step(err)) return 0;
  int64_t found = q->columnInt64(0);
  q->reset();
  return found;
}

int64_t PhotoDao::ensureFolder(int64_t rootId, const std::string& relPath, const std::string& name,
                               std::string* err) {
  std::string sql = "INSERT INTO folders(root_id,rel_path,name) VALUES(?,?,?) ON CONFLICT(root_id,"
                    "rel_path) DO NOTHING;";
  auto stmt = db_->prepareCached("ensureFolder", sql, err);
  if (!stmt || !stmt->valid()) return 0;
  std::vector<Bind> binds{Bind{Bind::kInt64, 0, 0, std::to_string(rootId)},
                          Bind{Bind::kText, 0, 0, relPath},
                          Bind{Bind::kText, 0, 0, name}};
  binds[0].kind = Bind::kInt64;
  binds[0].i = rootId;
  bindAll(stmt.get(), binds, err);
  if (!stmt->step(err)) return 0;
  sqlite3_int64 id = db_->lastInsertId();
  stmt->reset();
  if (id > 0) return static_cast<int64_t>(id);
  sql = "SELECT id FROM folders WHERE root_id=? AND rel_path=?;";
  auto q = db_->prepareCached("ensureFolderSelect", sql, err);
  if (!q || !q->valid()) return 0;
  std::vector<Bind> b2{Bind{Bind::kInt64, rootId, 0, std::string()}, Bind{Bind::kText, 0, 0, relPath}};
  bindAll(q.get(), b2, err);
  if (!q->step(err)) return 0;
  int64_t found = q->columnInt64(0);
  q->reset();
  return found;
}

std::vector<RootRecord> PhotoDao::listRoots(std::string* err) {
  std::vector<RootRecord> out;
  std::string sql = "SELECT id, path, label FROM roots ORDER BY id;";
  auto stmt = db_->prepareCached("listRoots", sql, err);
  if (!stmt || !stmt->valid()) return out;
  while (stmt->step(err)) {
    RootRecord r;
    r.id = stmt->columnInt64(0);
    r.path = stmt->columnText(1);
    r.label = stmt->columnText(2);
    out.push_back(r);
  }
  stmt->reset();
  return out;
}

std::vector<FolderInfo> PhotoDao::listFolders(std::string* err) {
  std::vector<FolderInfo> out;
  std::string sql =
      "SELECT f.id, f.root_id, f.rel_path, f.name, f.missing, "
      "(SELECT COUNT(*) FROM photos p WHERE p.folder_id=f.id AND p.deleted=0) AS c "
      "FROM folders f ORDER BY f.root_id, f.rel_path;";
  auto stmt = db_->prepareCached("listFolders", sql, err);
  if (!stmt || !stmt->valid()) return out;
  while (stmt->step(err)) {
    FolderInfo f;
    f.id = stmt->columnInt64(0);
    f.rootId = stmt->columnInt64(1);
    f.relPath = stmt->columnText(2);
    f.name = stmt->columnText(3);
    f.missing = stmt->columnInt(4) != 0;
    f.count = stmt->columnInt64(5);
    out.push_back(f);
  }
  stmt->reset();
  return out;
}

// ---------------- 写入 ----------------

bool PhotoDao::upsert(const PhotoRecord& p, int64_t* outId, std::string* err) {
  std::string sql = "INSERT INTO photos(folder_id,root_id,file_name,file_path,format,file_size,md5,"
                    "width,height,captured_at,imported_at,camera,lens,iso,aperture,shutter,"
                    "focal_length,white_balance,gps_lat,gps_lng,gps_alt,location_id,rating,pick,"
                    "color_label,keywords,description,sidecar_path,sidecar_mtime,has_edits,"
                    "is_missing,is_video,pair_kind,health,deleted,seq_id,location,date) "
                    "VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,"
                    "?,?) "
                    "ON CONFLICT(file_path) DO UPDATE SET "
                    "folder_id=excluded.folder_id, root_id=excluded.root_id, "
                    "file_name=excluded.file_name, format=excluded.format, "
                    "file_size=excluded.file_size, md5=excluded.md5, width=excluded.width, "
                    "height=excluded.height, captured_at=excluded.captured_at, "
                    "camera=excluded.camera, lens=excluded.lens, iso=excluded.iso, "
                    "aperture=excluded.aperture, shutter=excluded.shutter, "
                    "focal_length=excluded.focal_length, white_balance=excluded.white_balance, "
                    "gps_lat=excluded.gps_lat, gps_lng=excluded.gps_lng, gps_alt=excluded.gps_alt, "
                    "location_id=excluded.location_id, rating=excluded.rating, pick=excluded.pick, "
                    "color_label=excluded.color_label, keywords=excluded.keywords, "
                    "description=excluded.description, sidecar_path=excluded.sidecar_path, "
                    "sidecar_mtime=excluded.sidecar_mtime, has_edits=excluded.has_edits, "
                    "is_missing=excluded.is_missing, is_video=excluded.is_video, "
                    "pair_kind=excluded.pair_kind, health=excluded.health, "
                    "deleted=excluded.deleted, seq_id=excluded.seq_id, "
                    "location=excluded.location, date=excluded.date "
                    // 必须用 RETURNING 拿 id：走 ON CONFLICT DO UPDATE 分支时
                    // sqlite3_last_insert_rowid() 不会更新（保留 0 或上次 INSERT 的值），
                    // 会导致调用方拿到 photoId=0，进而 sidecar 写入全部失败。
                    "RETURNING id;";
  auto stmt = db_->prepareCached("upsertPhoto", sql, err);
  if (!stmt || !stmt->valid()) return false;
  std::vector<Bind> binds;
  auto b = [&binds](int64_t i) {
    Bind x;
    x.kind = Bind::kInt64;
    x.i = i;
    binds.push_back(x);
  };
  auto bt = [&binds](const std::string& s) {
    Bind x;
    x.kind = Bind::kText;
    x.s = s;
    binds.push_back(x);
  };
  b(p.folderId);
  b(p.rootId);
  bt(p.fileName);
  bt(p.filePath);
  bt(p.format);
  b(p.fileSize);
  bt(p.md5);
  b(p.width);
  b(p.height);
  b(p.capturedAt);
  b(p.importedAt);
  bt(p.camera);
  bt(p.lens);
  b(p.iso);
  bt(p.aperture);
  bt(p.shutter);
  bt(p.focalLength);
  bt(p.whiteBalance);
  if (p.hasGps) {
    Bind la;
    la.kind = Bind::kDouble;
    la.d = p.gpsLat;
    binds.push_back(la);
    Bind ln;
    ln.kind = Bind::kDouble;
    ln.d = p.gpsLng;
    binds.push_back(ln);
    Bind al;
    al.kind = Bind::kDouble;
    al.d = p.gpsAlt;
    binds.push_back(al);
  } else {
    for (int i = 0; i < 3; ++i) {
      Bind x;
      x.kind = Bind::kNull;
      binds.push_back(x);
    }
  }
  if (p.hasLocationId) {
    b(p.locationId);
  } else {
    Bind x;
    x.kind = Bind::kNull;
    binds.push_back(x);
  }
  b(p.rating);
  bt(p.pick);
  bt(p.colorLabel);
  bt(p.keywords);
  bt(p.description);
  bt(p.sidecarPath);
  b(p.sidecarMtime);
  b(p.hasEdits ? 1 : 0);
  b(p.isMissing ? 1 : 0);
  b(p.isVideo ? 1 : 0);
  bt(p.pairKind);
  bt(p.health);
  b(p.deleted ? 1 : 0);
  if (p.hasSeqId) {
    b(p.seqId);
  } else {
    Bind x;
    x.kind = Bind::kNull;
    binds.push_back(x);
  }
  bt(p.location);  // location（FTS 列：地点名）
  bt(p.date);      // date（FTS 列：YYYY-MM）
  bindAll(stmt.get(), binds, err);
  if (!stmt->step(err)) return false;
  int64_t newId = stmt->columnInt64(0);
  stmt->reset();
  if (outId) *outId = newId;
  return true;
}

bool PhotoDao::updateAll(const PhotoRecord& p, std::string* err) {
  return upsert(p, nullptr, err);
}

bool PhotoDao::getById(int64_t id, PhotoRecord* out, std::string* err) {
  std::string sql = "SELECT id,folder_id,root_id,file_name,file_path,format,file_size,md5,width,"
                    "height,captured_at,imported_at,camera,lens,iso,aperture,shutter,focal_length,"
                    "white_balance,gps_lat,gps_lng,gps_alt,location_id,rating,pick,color_label,"
                    "keywords,description,sidecar_path,sidecar_mtime,has_edits,is_missing,is_video,"
                    "pair_kind,health,deleted,seq_id,location,date FROM photos WHERE id=?;";
  auto stmt = db_->prepareCached("getPhotoById", sql, err);
  if (!stmt || !stmt->valid()) return false;
  std::vector<Bind> binds;
  binds.push_back(Bind{Bind::kInt64, id, 0, std::string()});
  bindAll(stmt.get(), binds, err);
  if (!stmt->step(err)) return false;
  out->id = stmt->columnInt64(0);
  out->folderId = stmt->columnInt64(1);
  out->rootId = stmt->columnInt64(2);
  out->fileName = stmt->columnText(3);
  out->filePath = stmt->columnText(4);
  out->format = stmt->columnText(5);
  out->fileSize = stmt->columnInt64(6);
  out->md5 = stmt->columnText(7);
  out->width = stmt->columnInt(8);
  out->height = stmt->columnInt(9);
  out->capturedAt = stmt->columnInt64(10);
  out->importedAt = stmt->columnInt64(11);
  out->camera = stmt->columnText(12);
  out->lens = stmt->columnText(13);
  out->iso = stmt->columnInt(14);
  out->aperture = stmt->columnText(15);
  out->shutter = stmt->columnText(16);
  out->focalLength = stmt->columnText(17);
  out->whiteBalance = stmt->columnText(18);
  out->hasGps = !stmt->columnIsNull(19) && !stmt->columnIsNull(20);
  out->gpsLat = stmt->columnDouble(19);
  out->gpsLng = stmt->columnDouble(20);
  out->gpsAlt = stmt->columnDouble(21);
  out->hasLocationId = !stmt->columnIsNull(22);
  out->locationId = stmt->columnInt64(22);
  out->rating = stmt->columnInt(23);
  out->pick = stmt->columnText(24);
  out->colorLabel = stmt->columnText(25);
  out->keywords = stmt->columnText(26);
  out->description = stmt->columnText(27);
  out->sidecarPath = stmt->columnText(28);
  out->hasSidecarMtime = !stmt->columnIsNull(29);
  out->sidecarMtime = stmt->columnInt64(29);
  out->hasEdits = stmt->columnInt(30) != 0;
  out->isMissing = stmt->columnInt(31) != 0;
  out->isVideo = stmt->columnInt(32) != 0;
  out->pairKind = stmt->columnText(33);
  out->health = stmt->columnText(34);
  out->deleted = stmt->columnInt(35) != 0;
  out->hasSeqId = !stmt->columnIsNull(36);
  out->seqId = stmt->columnInt64(36);
  out->location = stmt->columnText(37);
  out->date = stmt->columnText(38);
  stmt->reset();
  return true;
}

bool PhotoDao::getByPath(const std::string& path, PhotoRecord* out, std::string* err) {
  std::string sql = "SELECT id FROM photos WHERE file_path=?;";
  auto stmt = db_->prepareCached("getPhotoByPath", sql, err);
  if (!stmt || !stmt->valid()) return false;
  std::vector<Bind> binds;
  binds.push_back(Bind{Bind::kText, 0, 0, path});
  bindAll(stmt.get(), binds, err);
  if (!stmt->step(err)) return false;
  int64_t id = stmt->columnInt64(0);
  stmt->reset();
  return getById(id, out, err);
}

// ---------------- 查询 ----------------

namespace {

// 依据 Filter 生成 WHERE 片段（返回带占位符的 SQL 片段 + 绑定列表）
void buildWhere(const Filter& f, bool* hasCond, std::string* where, std::vector<Bind>* binds) {
  auto add = [&](const std::string& cond) {
    *where += (*hasCond ? " AND " : " WHERE ");
    *where += cond;
    *hasCond = true;
  };
  auto addInts = [&](const std::string& col, const std::vector<int64_t>& values) {
    std::string cond = col + " IN (";
    for (size_t i = 0; i < values.size(); ++i) {
      if (i) cond += ",";
      cond += "?";
      Bind b;
      b.kind = Bind::kInt64;
      b.i = values[i];
      binds->push_back(b);
    }
    cond += ")";
    add(cond);
  };
  auto addStrings = [&](const std::string& col, const std::vector<std::string>& values) {
    std::string cond = col + " IN (";
    for (size_t i = 0; i < values.size(); ++i) {
      if (i) cond += ",";
      cond += "?";
      Bind b;
      b.kind = Bind::kText;
      b.s = values[i];
      binds->push_back(b);
    }
    cond += ")";
    add(cond);
  };
  if (f.folderIds) addInts("p.folder_id", *f.folderIds);
  if (f.rootIds) addInts("p.root_id", *f.rootIds);
  if (f.formats) addStrings("p.format", *f.formats);
  if (f.cameras) addStrings("p.camera", *f.cameras);
  if (f.lenses) addStrings("p.lens", *f.lenses);
  if (f.minRating) {
    add("p.rating >= ?");
    Bind b;
    b.kind = Bind::kInt;
    b.i = *f.minRating;
    binds->push_back(b);
  }
  if (f.maxRating) {
    add("p.rating <= ?");
    Bind b;
    b.kind = Bind::kInt;
    b.i = *f.maxRating;
    binds->push_back(b);
  }
  if (f.pick) {
    add("p.pick = ?");
    Bind b;
    b.kind = Bind::kText;
    b.s = *f.pick;
    binds->push_back(b);
  }
  if (f.colorLabel) {
    add("p.color_label = ?");
    Bind b;
    b.kind = Bind::kText;
    b.s = *f.colorLabel;
    binds->push_back(b);
  }
  if (f.located) {
    add(*f.located ? "(p.gps_lat IS NOT NULL AND p.gps_lng IS NOT NULL)"
                   : "(p.gps_lat IS NULL OR p.gps_lng IS NULL)");
  }
  if (f.edited) add(*f.edited ? "p.has_edits = 1" : "p.has_edits = 0");
  if (f.missing) add(*f.missing ? "p.is_missing = 1" : "p.is_missing = 0");
  if (f.isVideo) add(*f.isVideo ? "p.is_video = 1" : "p.is_video = 0");
  if (f.dateFrom) {
    add("p.captured_at >= ?");
    Bind b;
    b.kind = Bind::kInt64;
    b.i = *f.dateFrom;
    binds->push_back(b);
  }
  if (f.dateTo) {
    add("p.captured_at <= ?");
    Bind b;
    b.kind = Bind::kInt64;
    b.i = *f.dateTo;
    binds->push_back(b);
  }
  if (f.locationIds) addInts("p.location_id", *f.locationIds);
  if (f.seqId) {
    add("p.seq_id = ?");
    Bind b;
    b.kind = Bind::kInt64;
    b.i = *f.seqId;
    binds->push_back(b);
  }
}

const char* kPhotoColumns =
    "p.id,p.folder_id,p.root_id,p.file_name,p.file_path,p.format,p.file_size,p.md5,p.width,p."
    "height,p.captured_at,p.imported_at,p.camera,p.lens,p.iso,p.aperture,p.shutter,p.focal_"
    "length,p.white_balance,p.gps_lat,p.gps_lng,p.gps_alt,p.location_id,p.rating,p.pick,p.color_"
    "label,p.keywords,p.description,p.sidecar_path,p.sidecar_mtime,p.has_edits,p.is_missing,p."
    "is_video,p.pair_kind,p.health,p.deleted,p.seq_id,p.location,p.date";

}  // namespace

bool PhotoDao::query(const Filter& f, const Sort& s, const Page& pg, QueryResult* out,
                     std::string* err) {
  bool hasCond = !f.includeDeleted;
  std::string where;
  std::vector<Bind> binds;
  if (!f.includeDeleted) where = " WHERE p.deleted=0";
  buildWhere(f, &hasCond, &where, &binds);

  // 总数
  std::string countSql = "SELECT COUNT(*) FROM photos p" + where + ";";
  auto countStmt = db_->prepareCached("queryPhotosCount", countSql, err);
  if (!countStmt || !countStmt->valid()) return false;
  bindAll(countStmt.get(), binds, err);
  if (!countStmt->step(err)) return false;
  out->total = countStmt->columnInt64(0);
  countStmt->reset();

  std::string orderCol = "p.captured_at";
  if (s.field == "importedAt") orderCol = "p.imported_at";
  else if (s.field == "fileName") orderCol = "p.file_name COLLATE NOCASE";
  else if (s.field == "rating") orderCol = "p.rating";
  else if (s.field == "capturedAt") orderCol = "p.captured_at";
  std::string orderBy = " ORDER BY " + orderCol + (s.desc ? " DESC" : " ASC") + ", p.id";
  // 两步取页（WP-A1 优化，5 万张库实测）：
  // 直接 LIMIT/OFFSET 取宽行时，SQLite 要对 offset+limit 每行做 40 列回表，
  // 深分页（offset≈4 万）p95 实测 452ms。改为：
  //   第 1 步只取本页 id（索引内扫描，无宽行回表，offset 扫描代价从 40 列降到 1 列）；
  //   第 2 步按 id 精确取回 limit 行宽行。回表行数从 offset+limit 降到 limit。
  std::string idSql = std::string("SELECT p.id FROM photos p") + where + orderBy +
                      " LIMIT ? OFFSET ?;";
  auto idStmt = db_->prepareCached("queryPhotosIds", idSql, err);
  if (!idStmt || !idStmt->valid()) return false;
  std::vector<Bind> pageBinds = binds;
  pageBinds.push_back(Bind{Bind::kInt, pg.limit, 0, std::string()});
  pageBinds.push_back(Bind{Bind::kInt, pg.offset, 0, std::string()});
  bindAll(idStmt.get(), pageBinds, err);
  std::string idList;
  while (idStmt->step(err)) {
    if (!idList.empty()) idList += ",";
    idList += std::to_string(idStmt->columnInt64(0));
  }
  idStmt->reset();
  out->items.clear();
  if (idList.empty()) return true;  // 页码越过总量：空页合法（total 已给出）

  // id 来自本库自增主键，可直接内联；排序键重申一次保证与第 1 步同序
  std::string dataSql = std::string("SELECT ") + kPhotoColumns + " FROM photos p WHERE p.id IN (" +
                        idList + ")" + orderBy + ";";
  auto stmt = db_->prepareCached("queryPhotosData", dataSql, err);
  if (!stmt || !stmt->valid()) return false;
  while (stmt->step(err)) {
    PhotoRecord p;
    p.id = stmt->columnInt64(0);
    p.folderId = stmt->columnInt64(1);
    p.rootId = stmt->columnInt64(2);
    p.fileName = stmt->columnText(3);
    p.filePath = stmt->columnText(4);
    p.format = stmt->columnText(5);
    p.fileSize = stmt->columnInt64(6);
    p.md5 = stmt->columnText(7);
    p.width = stmt->columnInt(8);
    p.height = stmt->columnInt(9);
    p.capturedAt = stmt->columnInt64(10);
    p.importedAt = stmt->columnInt64(11);
    p.camera = stmt->columnText(12);
    p.lens = stmt->columnText(13);
    p.iso = stmt->columnInt(14);
    p.aperture = stmt->columnText(15);
    p.shutter = stmt->columnText(16);
    p.focalLength = stmt->columnText(17);
    p.whiteBalance = stmt->columnText(18);
    p.hasGps = !stmt->columnIsNull(19) && !stmt->columnIsNull(20);
    p.gpsLat = stmt->columnDouble(19);
    p.gpsLng = stmt->columnDouble(20);
    p.gpsAlt = stmt->columnDouble(21);
    p.hasLocationId = !stmt->columnIsNull(22);
    p.locationId = stmt->columnInt64(22);
    p.rating = stmt->columnInt(23);
    p.pick = stmt->columnText(24);
    p.colorLabel = stmt->columnText(25);
    p.keywords = stmt->columnText(26);
    p.description = stmt->columnText(27);
    p.sidecarPath = stmt->columnText(28);
    p.hasSidecarMtime = !stmt->columnIsNull(29);
    p.sidecarMtime = stmt->columnInt64(29);
    p.hasEdits = stmt->columnInt(30) != 0;
    p.isMissing = stmt->columnInt(31) != 0;
    p.isVideo = stmt->columnInt(32) != 0;
    p.pairKind = stmt->columnText(33);
    p.health = stmt->columnText(34);
    p.deleted = stmt->columnInt(35) != 0;
    p.hasSeqId = !stmt->columnIsNull(36);
    p.seqId = stmt->columnInt64(36);
    p.location = stmt->columnText(37);
    p.date = stmt->columnText(38);
    out->items.push_back(p);
  }
  stmt->reset();
  return true;
}

bool PhotoDao::search(const std::string& text, const Filter& f, const Page& pg, QueryResult* out,
                      std::string* err) {
  // FTS5 匹配串：逐 token 加双引号，末 token 追加 '*'（前缀搜索）
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
    // 末 token 追加 '*'：FTS5 前缀查询写法是 "token"*（星号在引号外）。
    // 少了这一句，输入「四姑娘」永远匹配不到「四姑娘山」（unicode61 不切分中文）。
    if (i + 1 == tokens.size()) match += "*";
  }
  if (match.empty()) match = "\"\"";

  bool hasCond = false;
  std::string where;
  std::vector<Bind> binds;
  buildWhere(f, &hasCond, &where, &binds);

  std::string countSql = "SELECT COUNT(*) FROM photos_fts f JOIN photos p ON p.id=f.rowid" + where +
                         " AND photos_fts MATCH ?;";
  auto countStmt = db_->prepareCached("searchCount", countSql, err);
  if (!countStmt || !countStmt->valid()) return false;
  Bind matchBind;
  matchBind.kind = Bind::kText;
  matchBind.s = match;
  // bindAll 每次都从 ?1 开始绑定 → 必须一次性合并全部参数，
  // 否则后一批（MATCH）会覆盖前面的筛选条件（曾导致 search 计数/取数错乱）
  std::vector<Bind> countBinds = binds;
  countBinds.push_back(matchBind);
  bindAll(countStmt.get(), countBinds, err);
  if (!countStmt->step(err)) return false;
  out->total = countStmt->columnInt64(0);
  countStmt->reset();

  std::string orderCol = "p.captured_at";
  std::string dataSql = std::string("SELECT ") + kPhotoColumns +
                        " FROM photos_fts f JOIN photos p ON p.id=f.rowid" + where +
                        " AND photos_fts MATCH ? ORDER BY " + orderCol +
                        ", p.id LIMIT ? OFFSET ?;";
  auto stmt = db_->prepareCached("searchData", dataSql, err);
  if (!stmt || !stmt->valid()) return false;
  // 同 count：一次绑定全部参数（where + MATCH + limit + offset），
  // 分批调用会让 limit/offset 覆盖 MATCH，LIMIT 变 NULL → 永远 0 行
  std::vector<Bind> dataBinds = binds;
  dataBinds.push_back(matchBind);
  dataBinds.push_back(Bind{Bind::kInt, pg.limit, 0, std::string()});
  dataBinds.push_back(Bind{Bind::kInt, pg.offset, 0, std::string()});
  bindAll(stmt.get(), dataBinds, err);
  while (stmt->step(err)) {
    PhotoRecord p;
    p.id = stmt->columnInt64(0);
    p.folderId = stmt->columnInt64(1);
    p.rootId = stmt->columnInt64(2);
    p.fileName = stmt->columnText(3);
    p.filePath = stmt->columnText(4);
    p.format = stmt->columnText(5);
    p.fileSize = stmt->columnInt64(6);
    p.md5 = stmt->columnText(7);
    p.width = stmt->columnInt(8);
    p.height = stmt->columnInt(9);
    p.capturedAt = stmt->columnInt64(10);
    p.importedAt = stmt->columnInt64(11);
    p.camera = stmt->columnText(12);
    p.lens = stmt->columnText(13);
    p.iso = stmt->columnInt(14);
    p.aperture = stmt->columnText(15);
    p.shutter = stmt->columnText(16);
    p.focalLength = stmt->columnText(17);
    p.whiteBalance = stmt->columnText(18);
    p.hasGps = !stmt->columnIsNull(19) && !stmt->columnIsNull(20);
    p.gpsLat = stmt->columnDouble(19);
    p.gpsLng = stmt->columnDouble(20);
    p.gpsAlt = stmt->columnDouble(21);
    p.hasLocationId = !stmt->columnIsNull(22);
    p.locationId = stmt->columnInt64(22);
    p.rating = stmt->columnInt(23);
    p.pick = stmt->columnText(24);
    p.colorLabel = stmt->columnText(25);
    p.keywords = stmt->columnText(26);
    p.description = stmt->columnText(27);
    p.sidecarPath = stmt->columnText(28);
    p.hasSidecarMtime = !stmt->columnIsNull(29);
    p.sidecarMtime = stmt->columnInt64(29);
    p.hasEdits = stmt->columnInt(30) != 0;
    p.isMissing = stmt->columnInt(31) != 0;
    p.isVideo = stmt->columnInt(32) != 0;
    p.pairKind = stmt->columnText(33);
    p.health = stmt->columnText(34);
    p.deleted = stmt->columnInt(35) != 0;
    p.hasSeqId = !stmt->columnIsNull(36);
    p.seqId = stmt->columnInt64(36);
    p.location = stmt->columnText(37);
    p.date = stmt->columnText(38);
    out->items.push_back(p);
  }
  stmt->reset();
  return true;
}

int64_t PhotoDao::count(const Filter& f, std::string* err) {
  bool hasCond = false;
  std::string where;
  std::vector<Bind> binds;
  buildWhere(f, &hasCond, &where, &binds);
  std::string sql = "SELECT COUNT(*) FROM photos p" + where + ";";
  auto stmt = db_->prepareCached("countPhotos", sql, err);
  if (!stmt || !stmt->valid()) return -1;
  bindAll(stmt.get(), binds, err);
  if (!stmt->step(err)) return -1;
  int64_t n = stmt->columnInt64(0);
  stmt->reset();
  return n;
}

bool PhotoDao::listIds(const Filter& f, std::vector<int64_t>* out, std::string* err) {
  bool hasCond = false;
  std::string where;
  std::vector<Bind> binds;
  buildWhere(f, &hasCond, &where, &binds);
  std::string sql = "SELECT p.id FROM photos p" + where + " ORDER BY p.id;";
  auto stmt = db_->prepareCached("listIds", sql, err);
  if (!stmt || !stmt->valid()) return false;
  bindAll(stmt.get(), binds, err);
  while (stmt->step(err)) {
    out->push_back(stmt->columnInt64(0));
  }
  stmt->reset();
  return true;
}

// ---------------- 增量更新 ----------------

bool PhotoDao::updateFlags(const std::vector<int64_t>& ids, const json::JsonValue& patch,
                           int* updated, std::string* err) {
  *updated = 0;
  std::string sets;
  std::vector<Bind> binds;
  auto addSet = [&](const std::string& col, const std::string& value, Bind::Kind kind) {
    if (!sets.empty()) sets += ",";
    sets += col + "=?";
    Bind b;
    b.kind = kind;
    if (kind == Bind::kText) b.s = value;
    else b.i = std::stoll(value);
    binds.push_back(b);
  };
  if (patch.contains("rating")) {
    int r = static_cast<int>(patch.get("rating").asInt(0));
    if (r < 0) r = 0;
    if (r > 5) r = 5;
    addSet("rating", std::to_string(r), Bind::kInt);
  }
  if (patch.contains("pick")) addSet("pick", patch.get("pick").asString("none"), Bind::kText);
  if (patch.contains("colorLabel")) {
    std::string label = patch.get("colorLabel").asString();
    if (label.empty() || patch.get("colorLabel").isNull()) {
      sets += sets.empty() ? "" : ",";
      sets += "color_label=NULL";
    } else {
      addSet("color_label", label, Bind::kText);
    }
  }
  if (patch.contains("description")) {
    addSet("description", patch.get("description").asString(), Bind::kText);
  }
  if (patch.contains("keywords")) {
    std::string joined;
    const json::JsonValue& kw = patch.get("keywords");
    if (kw.isArray()) {
      bool first = true;
      for (const auto& item : kw.arrayItems()) {
        if (!first) joined += ";";
        joined += item.asString();
        first = false;
      }
    } else {
      joined = kw.asString();
    }
    addSet("keywords", joined, Bind::kText);
  }
  if (sets.empty()) {
    *updated = static_cast<int>(ids.size());
    return true;
  }
  for (int64_t id : ids) {
    std::string sql = "UPDATE photos SET " + sets + " WHERE id=?;";
    auto stmt = db_->prepareCached("updateFlags", sql, err);
    if (!stmt || !stmt->valid()) return false;
    // 一次性绑定：SET 值 + WHERE id（分两次会让 id 覆盖第一个 SET 值）
    std::vector<Bind> stmtBinds = binds;
    stmtBinds.push_back(Bind{Bind::kInt64, id, 0, std::string()});
    bindAll(stmt.get(), stmtBinds, err);
    if (!stmt->step(err)) return false;
    *updated += db_->changes();
    stmt->reset();
  }
  return true;
}

bool PhotoDao::softRemove(const std::vector<int64_t>& ids, int* removed, std::string* err) {
  *removed = 0;
  if (ids.empty()) return true;
  std::string sql = "UPDATE photos SET deleted=1, imported_at=imported_at WHERE id=?;";
  auto stmt = db_->prepareCached("softRemove", sql, err);
  if (!stmt || !stmt->valid()) return false;
  for (int64_t id : ids) {
    bindAll(stmt.get(), std::vector<Bind>{Bind{Bind::kInt64, id, 0, std::string()}}, err);
    if (!stmt->step(err)) return false;
    *removed += db_->changes();
    stmt->reset();
  }
  return true;
}

bool PhotoDao::restore(const std::vector<int64_t>& ids, int* restored, std::string* err) {
  *restored = 0;
  if (ids.empty()) return true;
  std::string sql = "UPDATE photos SET deleted=0 WHERE id=?;";
  auto stmt = db_->prepareCached("restorePhotos", sql, err);
  if (!stmt || !stmt->valid()) return false;
  for (int64_t id : ids) {
    bindAll(stmt.get(), std::vector<Bind>{Bind{Bind::kInt64, id, 0, std::string()}}, err);
    if (!stmt->step(err)) return false;
    *restored += db_->changes();
    stmt->reset();
  }
  return true;
}

bool PhotoDao::setMissing(int64_t id, bool missing, std::string* err) {
  std::string sql = "UPDATE photos SET is_missing=? WHERE id=?;";
  auto stmt = db_->prepareCached("setMissing", sql, err);
  if (!stmt || !stmt->valid()) return false;
  bindAll(stmt.get(), std::vector<Bind>{Bind{Bind::kInt, missing ? 1 : 0, 0, std::string()},
                                        Bind{Bind::kInt64, id, 0, std::string()}},
          err);
  if (!stmt->step(err)) return false;
  stmt->reset();
  return true;
}

bool PhotoDao::setHealth(int64_t id, const std::string& health, std::string* err) {
  std::string sql = "UPDATE photos SET health=? WHERE id=?;";
  auto stmt = db_->prepareCached("setHealth", sql, err);
  if (!stmt || !stmt->valid()) return false;
  bindAll(stmt.get(), std::vector<Bind>{Bind{Bind::kText, 0, 0, health},
                                        Bind{Bind::kInt64, id, 0, std::string()}},
          err);
  if (!stmt->step(err)) return false;
  stmt->reset();
  return true;
}

bool PhotoDao::setMd5(int64_t id, const std::string& md5, std::string* err) {
  std::string sql = "UPDATE photos SET md5=? WHERE id=?;";
  auto stmt = db_->prepareCached("setMd5", sql, err);
  if (!stmt || !stmt->valid()) return false;
  bindAll(stmt.get(), std::vector<Bind>{Bind{Bind::kText, 0, 0, md5},
                                        Bind{Bind::kInt64, id, 0, std::string()}},
          err);
  if (!stmt->step(err)) return false;
  stmt->reset();
  return true;
}

bool PhotoDao::setSidecar(int64_t id, const std::string& sidecarPath, int64_t mtime, bool hasEdits,
                          std::string* err) {
  std::string sql = "UPDATE photos SET sidecar_path=?, sidecar_mtime=?, has_edits=? WHERE id=?;";
  auto stmt = db_->prepareCached("setSidecar", sql, err);
  if (!stmt || !stmt->valid()) return false;
  bindAll(stmt.get(), std::vector<Bind>{Bind{Bind::kText, 0, 0, sidecarPath},
                                        Bind{Bind::kInt64, mtime, 0, std::string()},
                                        Bind{Bind::kInt, hasEdits ? 1 : 0, 0, std::string()},
                                        Bind{Bind::kInt64, id, 0, std::string()}},
          err);
  if (!stmt->step(err)) return false;
  stmt->reset();
  return true;
}

bool PhotoDao::setGps(int64_t id, double lat, double lng, double alt, const std::string& locationName,
                      std::string* err) {
  std::string sql = "UPDATE photos SET gps_lat=?, gps_lng=?, gps_alt=?, location=? WHERE id=?;";
  auto stmt = db_->prepareCached("setGps", sql, err);
  if (!stmt || !stmt->valid()) return false;
  bindAll(stmt.get(),
          std::vector<Bind>{Bind{Bind::kDouble, 0, lat, std::string()},
                            Bind{Bind::kDouble, 0, lng, std::string()},
                            Bind{Bind::kDouble, 0, alt, std::string()},
                            Bind{Bind::kText, 0, 0, locationName},
                            Bind{Bind::kInt64, id, 0, std::string()}},
          err);
  if (!stmt->step(err)) return false;
  stmt->reset();
  return true;
}

bool PhotoDao::setSeqId(int64_t id, int64_t seqId, std::string* err) {
  std::string sql = "UPDATE photos SET seq_id=? WHERE id=?;";
  auto stmt = db_->prepareCached("setSeqId", sql, err);
  if (!stmt || !stmt->valid()) return false;
  bindAll(stmt.get(), std::vector<Bind>{Bind{Bind::kInt64, seqId, 0, std::string()},
                                        Bind{Bind::kInt64, id, 0, std::string()}},
          err);
  if (!stmt->step(err)) return false;
  stmt->reset();
  return true;
}

bool PhotoDao::setPairKind(int64_t id, const std::string& pairKind, std::string* err) {
  std::string sql = "UPDATE photos SET pair_kind=? WHERE id=?;";
  auto stmt = db_->prepareCached("setPairKind", sql, err);
  if (!stmt || !stmt->valid()) return false;
  bindAll(stmt.get(), std::vector<Bind>{Bind{Bind::kText, 0, 0, pairKind},
                                        Bind{Bind::kInt64, id, 0, std::string()}},
          err);
  if (!stmt->step(err)) return false;
  stmt->reset();
  return true;
}

bool PhotoDao::setBackup(int64_t id, const std::string& backupPath, int64_t verifiedAt,
                         std::string* err) {
  std::string sql = "INSERT INTO backups(photo_id, backup_path, verified_at) VALUES(?,?,?) "
                    "ON CONFLICT(photo_id) DO UPDATE SET backup_path=excluded.backup_path, "
                    "verified_at=excluded.verified_at;";
  auto stmt = db_->prepareCached("setBackup", sql, err);
  if (!stmt || !stmt->valid()) return false;
  bindAll(stmt.get(), std::vector<Bind>{Bind{Bind::kInt64, id, 0, std::string()},
                                        Bind{Bind::kText, 0, 0, backupPath},
                                        Bind{Bind::kInt64, verifiedAt, 0, std::string()}},
          err);
  if (!stmt->step(err)) return false;
  stmt->reset();
  return true;
}

bool PhotoDao::deletePermanently(const std::vector<int64_t>& ids, std::string* err) {
  if (ids.empty()) return true;
  std::string sql = "DELETE FROM photos WHERE id=?;";
  auto stmt = db_->prepareCached("deletePhoto", sql, err);
  if (!stmt || !stmt->valid()) return false;
  for (int64_t id : ids) {
    bindAll(stmt.get(), std::vector<Bind>{Bind{Bind::kInt64, id, 0, std::string()}}, err);
    if (!stmt->step(err)) return false;
    stmt->reset();
  }
  return true;
}

// ---------------- 聚合 ----------------

bool PhotoDao::facets(const Filter& f, json::JsonValue* out, std::string* err) {
  bool hasCond = false;
  std::string where;
  std::vector<Bind> binds;
  buildWhere(f, &hasCond, &where, &binds);
  auto group = [&](const char* col, json::JsonValue* target) -> bool {
    std::string sql = "SELECT " + std::string(col) +
                      ", COUNT(*) c FROM photos p" + where +
                      " GROUP BY " + std::string(col) + " ORDER BY c DESC, " + col + ";";
    auto stmt = db_->prepareCached(std::string("facet_") + col, sql, err);
    if (!stmt || !stmt->valid()) return false;
    bindAll(stmt.get(), binds, err);
    while (stmt->step(err)) {
      json::JsonValue item = json::JsonValue::objectValue();
      item.setItem("value", stmt->columnIsNull(0) ? std::string("") : stmt->columnText(0));
      item.setItem("count", stmt->columnInt64(1));
      target->addItem(item);
    }
    stmt->reset();
    return true;
  };
  // get() 返回 const 引用，故先聚合到局部数组再整体挂到 out（避免 const 绕行）
  json::JsonValue cameras = json::JsonValue::arrayValue();
  json::JsonValue lenses = json::JsonValue::arrayValue();
  json::JsonValue formats = json::JsonValue::arrayValue();
  json::JsonValue ratings = json::JsonValue::arrayValue();
  json::JsonValue locations = json::JsonValue::arrayValue();
  if (!group("camera", &cameras)) return false;
  if (!group("lens", &lenses)) return false;
  if (!group("format", &formats)) return false;
  if (!group("rating", &ratings)) return false;
  if (!group("location", &locations)) return false;
  out->setItem("cameras", cameras);
  out->setItem("lenses", lenses);
  out->setItem("formats", formats);
  out->setItem("ratings", ratings);
  out->setItem("locations", locations);
  return true;
}

bool PhotoDao::timeline(const Filter& f, std::vector<MonthBucket>* out, std::string* err) {
  bool hasCond = false;
  std::string where;
  std::vector<Bind> binds;
  buildWhere(f, &hasCond, &where, &binds);
  std::string sql = "SELECT date, COUNT(*) c, MIN(p.id) firstId FROM photos p" + where +
                    " GROUP BY date ORDER BY date;";
  auto stmt = db_->prepareCached("timeline", sql, err);
  if (!stmt || !stmt->valid()) return false;
  bindAll(stmt.get(), binds, err);
  while (stmt->step(err)) {
    MonthBucket b;
    b.month = stmt->columnText(0);
    b.count = stmt->columnInt64(1);
    b.firstPhotoId = stmt->columnInt64(2);
    if (!b.month.empty()) out->push_back(b);
  }
  stmt->reset();
  return true;
}

bool PhotoDao::stats(json::JsonValue* out, std::string* err) {
  std::string sql =
      "SELECT COUNT(*), COALESCE(SUM(file_size),0), "
      "COALESCE(SUM(CASE WHEN is_missing=1 THEN 1 ELSE 0 END),0), "
      "COALESCE(SUM(CASE WHEN has_edits=1 THEN 1 ELSE 0 END),0) "
      "FROM photos WHERE deleted=0;";
  auto stmt = db_->prepareCached("statsTotal", sql, err);
  if (!stmt || !stmt->valid()) return false;
  if (!stmt->step(err)) return false;
  int64_t total = stmt->columnInt64(0);
  int64_t bytes = stmt->columnInt64(1);
  int64_t missing = stmt->columnInt64(2);
  int64_t edited = stmt->columnInt64(3);
  stmt->reset();
  // 健康已校验（health='ok'）与损坏数
  sql = "SELECT COALESCE(SUM(CASE WHEN health='ok' THEN 1 ELSE 0 END),0), "
        "COALESCE(SUM(CASE WHEN health='corrupt' THEN 1 ELSE 0 END),0) FROM photos WHERE deleted=0;";
  auto hs = db_->prepareCached("statsHealth", sql, err);
  if (!hs || !hs->valid()) return false;
  if (!hs->step(err)) return false;
  int64_t verified = hs->columnInt64(0);
  int64_t corrupt = hs->columnInt64(1);
  hs->reset();

  out->setItem("totalPhotos", total);
  out->setItem("totalBytes", bytes);
  out->setItem("verified", verified);
  out->setItem("missing", missing);
  out->setItem("edited", edited);
  out->setItem("corrupt", corrupt);
  json::JsonValue byFormat = json::JsonValue::objectValue();
  sql = "SELECT format, COUNT(*) c FROM photos WHERE deleted=0 GROUP BY format;";
  auto fs = db_->prepareCached("statsByFormat", sql, err);
  if (!fs || !fs->valid()) return false;
  while (fs->step(err)) {
    byFormat.setItem(fs->columnText(0), fs->columnInt64(1));
  }
  fs->reset();
  out->setItem("byFormat", byFormat);
  json::JsonValue months = json::JsonValue::arrayValue();
  sql = "SELECT date, COUNT(*) c FROM photos WHERE deleted=0 AND date IS NOT NULL AND date<>'' "
        "GROUP BY date ORDER BY date;";
  auto ms = db_->prepareCached("statsMonths", sql, err);
  if (!ms || !ms->valid()) return false;
  while (ms->step(err)) {
    json::JsonValue m = json::JsonValue::objectValue();
    m.setItem("month", ms->columnText(0));
    m.setItem("count", ms->columnInt64(1));
    months.addItem(m);
  }
  ms->reset();
  out->setItem("months", months);
  return true;
}

bool PhotoDao::byFormat(json::JsonValue* out, std::string* err) {
  std::string sql = "SELECT format, COUNT(*) c FROM photos WHERE deleted=0 GROUP BY format;";
  auto stmt = db_->prepareCached("byFormatOnly", sql, err);
  if (!stmt || !stmt->valid()) return false;
  *out = json::JsonValue::objectValue();
  while (stmt->step(err)) {
    out->setItem(stmt->columnText(0), stmt->columnInt64(1));
  }
  stmt->reset();
  return true;
}

}  // namespace db
}  // namespace lumina
