// M2 T2.2 · 图像身份实现（四级表解析 + 移动/重命名 id 稳定性）
//
// 关键不变量（test_image_identity 逐条断言）：
//   I1  resolveByFile(root, rel, name) 对同一逻辑文件永远返回同一个 photos.id；
//   I2  relocate 只改路径列与 folder 归属，photos.id / files.id 不变；
//   I3  迁移幂等：第二次启动 0 变更；
//   I4  photos_fts 仍可检索（本层从不重建 photos 表，也不碰 FTS 触发器）。
// 全部 SQL 走绑定参数（文件名可能含引号/中文，禁止拼串）。
#include "db/image_dao.h"

#include <string>
#include <vector>

#include "core/logger.h"
#include "core/str_util.h"

namespace lumina {
namespace db {
namespace {

// 绑定值载体（按占位符顺序绑定）
struct Bind {
  enum Kind { kInt64, kText } kind = kInt64;
  int64_t i = 0;
  std::string s;
  static Bind num(int64_t v) {
    Bind b;
    b.kind = kInt64;
    b.i = v;
    return b;
  }
  static Bind text(const std::string& v) {
    Bind b;
    b.kind = kText;
    b.s = v;
    return b;
  }
};

bool bindAll(Stmt* stmt, const std::vector<Bind>& binds, std::string* err) {
  for (size_t idx = 0; idx < binds.size(); ++idx) {
    const int pos = static_cast<int>(idx) + 1;
    const Bind& b = binds[idx];
    const bool ok =
        (b.kind == Bind::kInt64) ? stmt->bindInt64(pos, b.i) : stmt->bindText(pos, b.s);
    if (!ok) {
      if (err) *err = "bind failed at " + std::to_string(pos);
      return false;
    }
  }
  return true;
}

// 单列 int64 查询；无行返回 0（调用方按 0 = 未命中处理）
int64_t queryId(Database* db, const std::string& sql, const std::vector<Bind>& binds,
                const std::string& errCtx, std::string* err) {
  auto stmt = db->prepareCached("imgdao:" + errCtx, sql, err);
  if (!stmt || !stmt->valid()) return 0;
  if (!bindAll(stmt.get(), binds, err)) return 0;
  int64_t v = 0;
  if (stmt->step(err)) v = stmt->columnInt64(0);
  stmt->reset();
  return v;
}

// 单列文本查询；无行返回空串
std::string queryText(Database* db, const std::string& sql, const std::vector<Bind>& binds,
                      const std::string& errCtx, std::string* err) {
  auto stmt = db->prepareCached("imgdao:" + errCtx, sql, err);
  if (!stmt || !stmt->valid()) return std::string();
  if (!bindAll(stmt.get(), binds, err)) return std::string();
  std::string v;
  if (stmt->step(err)) v = stmt->columnText(0);
  stmt->reset();
  return v;
}

}  // namespace

bool ImageDao::hasFileIdentity(std::string* err) const {
  if (!db_ || !db_->isOpen()) {
    if (err) *err = "image_dao: database not open";
    return false;
  }
  auto stmt = db_->prepareCached("imgdao:pragmaPhotos", "PRAGMA table_info(photos);", err);
  if (!stmt || !stmt->valid()) return false;
  bool found = false;
  while (stmt->step(nullptr)) {
    if (stmt->columnText(1) == "file_id") {
      found = true;
      break;
    }
  }
  stmt->reset();
  if (!found && err) {
    *err = "image_dao: photos.file_id 不存在（迁移 002 未执行）";
  }
  return found;
}

int64_t ImageDao::findFolderId(int64_t rootId, const std::string& relPath, std::string* err) {
  if (!db_ || !db_->isOpen()) {
    if (err) *err = "image_dao: database not open";
    return 0;
  }
  return queryId(db_, "SELECT id FROM folders WHERE root_id=? AND rel_path=? LIMIT 1;",
                 {Bind::num(rootId), Bind::text(relPath)}, "findFolder", err);
}

int64_t ImageDao::ensureFolder(int64_t rootId, const std::string& relPath, std::string* err) {
  if (!db_ || !db_->isOpen()) {
    if (err) *err = "image_dao: database not open";
    return 0;
  }
  if (rootId <= 0) {
    if (err) *err = "image_dao: rootId 非法";
    return 0;
  }
  const int64_t existing = findFolderId(rootId, relPath, err);
  if (existing > 0) return existing;
  const std::string name = relPath.empty() ? std::string("/") : baseName(relPath);
  auto stmt = db_->prepareCached("imgdao:ensureFolder",
                                 "INSERT OR IGNORE INTO folders(root_id,rel_path,name) VALUES(?,?,?);",
                                 err);
  if (!stmt || !stmt->valid()) return 0;
  if (!bindAll(stmt.get(), {Bind::num(rootId), Bind::text(relPath), Bind::text(name)}, err)) {
    return 0;
  }
  if (!stmt->step(err)) return 0;
  stmt->reset();
  return findFolderId(rootId, relPath, err);
}

std::string ImageDao::rootPath(int64_t rootId, std::string* err) {
  if (!db_ || !db_->isOpen()) {
    if (err) *err = "image_dao: database not open";
    return std::string();
  }
  return queryText(db_, "SELECT path FROM roots WHERE id=? LIMIT 1;", {Bind::num(rootId)},
                   "rootPath", err);
}

int64_t ImageDao::ensureFileRow(int64_t folderId, const std::string& name, std::string* err) {
  if (!db_ || !db_->isOpen()) {
    if (err) *err = "image_dao: database not open";
    return 0;
  }
  if (folderId <= 0 || name.empty()) {
    if (err) *err = "image_dao: ensureFileRow 参数非法（folderId/name）";
    return 0;
  }
  if (!hasFileIdentity(err)) return 0;  // 零静默：迁移没跑就明确失败
  auto stmt = db_->prepareCached("imgdao:ensureFile",
                                 "INSERT OR IGNORE INTO files(folder_id,name) VALUES(?,?);", err);
  if (!stmt || !stmt->valid()) return 0;
  if (!bindAll(stmt.get(), {Bind::num(folderId), Bind::text(name)}, err)) return 0;
  if (!stmt->step(err)) return 0;
  stmt->reset();
  return queryId(db_, "SELECT id FROM files WHERE folder_id=? AND name=? LIMIT 1;",
                 {Bind::num(folderId), Bind::text(name)}, "findFile", err);
}

int64_t ImageDao::fileIdOfPhoto(int64_t photoId, std::string* err) {
  if (!db_ || !db_->isOpen()) {
    if (err) *err = "image_dao: database not open";
    return 0;
  }
  if (!hasFileIdentity(err)) return 0;
  return queryId(db_, "SELECT COALESCE(file_id,0) FROM photos WHERE id=? LIMIT 1;",
                 {Bind::num(photoId)}, "fileIdOfPhoto", err);
}

int64_t ImageDao::photoIdByFile(int64_t fileId, std::string* err) {
  if (!db_ || !db_->isOpen()) {
    if (err) *err = "image_dao: database not open";
    return 0;
  }
  if (fileId <= 0) return 0;
  if (!hasFileIdentity(err)) return 0;
  // 未删除优先（软删除的行保留 file_id，但不该被路径解析优先命中）
  return queryId(db_,
                 "SELECT id FROM photos WHERE file_id=? ORDER BY deleted ASC, id ASC LIMIT 1;",
                 {Bind::num(fileId)}, "photoIdByFile", err);
}

std::string ImageDao::fileNameOfPhoto(int64_t photoId, std::string* err) {
  if (!db_ || !db_->isOpen()) {
    if (err) *err = "image_dao: database not open";
    return std::string();
  }
  return queryText(db_, "SELECT file_name FROM photos WHERE id=? LIMIT 1;", {Bind::num(photoId)},
                   "fileNameOfPhoto", err);
}

bool ImageDao::linkPhoto(int64_t photoId, int64_t fileId, std::string* err) {
  if (!db_ || !db_->isOpen()) {
    if (err) *err = "image_dao: database not open";
    return false;
  }
  if (photoId <= 0 || fileId <= 0) {
    if (err) *err = "image_dao: linkPhoto 参数非法";
    return false;
  }
  if (!hasFileIdentity(err)) return false;
  const int64_t cur = fileIdOfPhoto(photoId, err);
  if (cur == fileId) return true;  // 幂等：已绑定同一行，不产生写入
  auto stmt = db_->prepareCached("imgdao:linkPhoto", "UPDATE photos SET file_id=? WHERE id=?;", err);
  if (!stmt || !stmt->valid()) return false;
  if (!bindAll(stmt.get(), {Bind::num(fileId), Bind::num(photoId)}, err)) return false;
  if (!stmt->step(err)) return false;
  stmt->reset();
  return true;
}

int64_t ImageDao::resolveByFile(int64_t rootId, const std::string& relPath,
                                const std::string& name, std::string* err) {
  if (!db_ || !db_->isOpen()) {
    if (err) *err = "image_dao: database not open";
    return 0;
  }
  if (name.empty()) {
    if (err) *err = "image_dao: 文件名为空";
    return 0;
  }
  const int64_t folderId = findFolderId(rootId, relPath, err);
  if (folderId <= 0) {
    if (err) {
      *err = "image_dao: 文件夹不在库中（root=" + std::to_string(rootId) + ", rel=" + relPath + "）";
    }
    return 0;
  }
  // ① 文件身份命中 → 照片
  const int64_t fileId = queryId(db_, "SELECT id FROM files WHERE folder_id=? AND name=? LIMIT 1;",
                                 {Bind::num(folderId), Bind::text(name)}, "resolveFile", err);
  if (fileId > 0) {
    const int64_t photoId = photoIdByFile(fileId, err);
    if (photoId > 0) return photoId;
  }
  // ② 迁移前插入的旧行：按 (folder_id, file_name) 回退命中，并顺手补齐文件身份
  const int64_t legacyId = queryId(
      db_, "SELECT id FROM photos WHERE folder_id=? AND file_name=? ORDER BY deleted ASC, id ASC "
           "LIMIT 1;",
      {Bind::num(folderId), Bind::text(name)}, "resolveLegacy", err);
  if (legacyId > 0) {
    const int64_t fid = fileId > 0 ? fileId : ensureFileRow(folderId, name, err);
    if (fid > 0) {
      if (!linkPhoto(legacyId, fid, err)) return 0;
    } else if (err) {
      *err = "image_dao: 旧行命中但文件身份建立失败（photo=" + std::to_string(legacyId) + "）";
      return 0;
    }
    logInfo("image_dao", "lazily linked photo " + std::to_string(legacyId) + " to file " +
                             std::to_string(fid) + " (" + name + ")");
    return legacyId;
  }
  if (err) *err = "image_dao: 未找到图像（" + relPath + "/" + name + "）";
  return 0;
}

bool ImageDao::relocate(int64_t photoId, int64_t rootId, const std::string& relPath,
                        std::string* err) {
  if (!db_ || !db_->isOpen()) {
    if (err) *err = "image_dao: database not open";
    return false;
  }
  if (photoId <= 0) {
    if (err) *err = "image_dao: photoId 非法";
    return false;
  }
  if (!hasFileIdentity(err)) return false;
  const std::string name = fileNameOfPhoto(photoId, err);
  if (name.empty()) {
    if (err) *err = "image_dao: 照片不存在（id=" + std::to_string(photoId) + "）";
    return false;
  }
  const std::string root = rootPath(rootId, err);
  if (root.empty()) {
    if (err) *err = "image_dao: 根不存在（id=" + std::to_string(rootId) + "）";
    return false;
  }
  const int64_t folderId = ensureFolder(rootId, relPath, err);
  if (folderId <= 0) {
    if (err) *err = "image_dao: 目标文件夹创建失败（" + relPath + "）";
    return false;
  }
  const std::string newPath =
      relPath.empty() ? joinPath(root, name) : joinPath(joinPath(root, relPath), name);
  // 冲突检查：目标路径已被另一张照片占用 → 明确失败（不静默覆盖别人的行）
  const int64_t conflict = queryId(db_, "SELECT id FROM photos WHERE file_path=? AND id<>? LIMIT 1;",
                                   {Bind::text(newPath), Bind::num(photoId)}, "relocateConflict", err);
  if (conflict > 0) {
    if (err) {
      *err = "image_dao: 目标路径已被照片 " + std::to_string(conflict) + " 占用（" + newPath + "）";
    }
    return false;
  }
  const int64_t fileId = ensureFileRow(folderId, name, err);
  if (fileId <= 0) {
    if (err) *err = "image_dao: 目标文件身份创建失败（" + name + "）";
    return false;
  }
  auto stmt = db_->prepareCached(
      "imgdao:relocate",
      "UPDATE photos SET folder_id=?, root_id=?, file_path=?, file_id=? WHERE id=?;", err);
  if (!stmt || !stmt->valid()) return false;
  if (!bindAll(stmt.get(),
               {Bind::num(folderId), Bind::num(rootId), Bind::text(newPath), Bind::num(fileId),
                Bind::num(photoId)},
               err)) {
    return false;
  }
  if (!stmt->step(err)) return false;
  stmt->reset();
  // photos.id 不变是硬不变量：写入后自检（异常路径必须可见，不得静默）
  const int64_t stillThere = queryId(db_, "SELECT id FROM photos WHERE id=? AND file_id=? LIMIT 1;",
                                     {Bind::num(photoId), Bind::num(fileId)}, "relocateVerify", err);
  if (stillThere != photoId) {
    if (err) *err = "image_dao: relocate 后 photos.id 不稳定（期望 " + std::to_string(photoId) +
                    "，实得 " + std::to_string(stillThere) + "）";
    logError("image_dao", "relocate id instability detected");
    return false;
  }
  return true;
}

}  // namespace db
}  // namespace lumina
