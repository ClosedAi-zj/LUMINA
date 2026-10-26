// M2 T2.2 · 图像身份接口（四级表之上的解析层，Q1）
//
// 身份模型（架构 §3.3.1 语义表）：
//   根身份   roots(path)
//   文件夹   folders(root_id, rel_path)
//   文件     files(folder_id, name)        ← 本层新增
//   图像     photos.id                     ← 永不变（移动/重命名只改路径列）
//
// 本层只做「路径 → 身份」的解析与「移动/重命名」的搬迁，**不碰像素、不碰参数**。
// 参数缓存（develop_settings）与 history 都依赖本层给出的稳定 photos.id（T2.3/T2.4）。
#ifndef LUMINA_DB_IMAGE_DAO_H_
#define LUMINA_DB_IMAGE_DAO_H_

#include <atomic>
#include <cstdint>
#include <string>

#include "db/database.h"

namespace lumina {
namespace xmp {
class XmpService;  // 只用 sidecarPathFor：前向声明避免 db→xmp 头依赖
}

namespace db {

class ImageDao {
 public:
  explicit ImageDao(Database* db) : db_(db) {}
  ImageDao(Database* db, xmp::XmpService* xmp) : db_(db), xmp_(xmp) {}

  Database* database() const { return db_; }

  // 迁移是否已跑（photos.file_id 列是否存在）。未跑时所有 file_id 相关操作必须
  // 显式失败，不得静默降级成「按 file_path 猜」。
  bool hasFileIdentity(std::string* err) const;

  // (root, rel_path, name) → photos.id。未命中返回 0 并写 err（零静默）。
  // 命中路径：files(folder_id,name) → photos.file_id；若该照片尚未绑定 file_id
  // （迁移前插入的旧行），按 (folder_id, file_name) 回退命中并**顺手补齐绑定**。
  int64_t resolveByFile(int64_t rootId, const std::string& relPath, const std::string& name,
                        std::string* err);

  // 移动/重命名：只改 folders.rel_path（目标文件夹）+ photos 的
  // folder_id/root_id/file_path/file_id，**photos.id 不变**。
  // 目标 file_path 已被另一张照片占用 → 返回 false 并写 err（不静默覆盖别人的行）。
  bool relocate(int64_t photoId, int64_t rootId, const std::string& relPath, std::string* err);

  // 确保 files 行存在（幂等），返回 file id；folder_id/name 非法返回 0 + err
  int64_t ensureFileRow(int64_t folderId, const std::string& name, std::string* err);

  // 把照片绑定到文件行（幂等；已绑定同一行则不产生写入）
  bool linkPhoto(int64_t photoId, int64_t fileId, std::string* err);

  // (root, rel_path) → folders.id；不存在返回 0（不创建）
  int64_t findFolderId(int64_t rootId, const std::string& relPath, std::string* err);

  // (root, rel_path) → folders.id；不存在则创建（搬迁目标文件夹时用）
  int64_t ensureFolder(int64_t rootId, const std::string& relPath, std::string* err);

  // 读取辅助
  int64_t fileIdOfPhoto(int64_t photoId, std::string* err = nullptr);
  int64_t photoIdByFile(int64_t fileId, std::string* err = nullptr);
  std::string rootPath(int64_t rootId, std::string* err = nullptr);
  std::string fileNameOfPhoto(int64_t photoId, std::string* err = nullptr);

  // M2 T2.2b（启动兜底）：files 表一致性重建 —— 对齐 photos 与 files 表。
  // 对每张未删除照片：确保 (folder_id, file_name) 的 files 行存在，并把
  // photos.file_id 修指向该行（NULL/悬空都算陈旧）。返回关联/修复行数。
  // 幂等：第二次运行应为 0。可取消（启动后台线程调用，不阻塞握手）。
  struct ReconcileReport {
    int64_t photosScanned = 0;
    int64_t filesInserted = 0;
    int64_t photosRelinked = 0;
    int64_t photosUnlinked = 0;  // folder_id 缺失等无法归位的行（不静默，进 detail）
    std::string detail;
  };
  bool reconcile(ReconcileReport* report, std::string* err, std::atomic<bool>* cancel = nullptr);

 private:
  Database* db_;
  xmp::XmpService* xmp_ = nullptr;  // N10：relocate 重算 sidecar_path 用（可为空）
};

}  // namespace db
}  // namespace lumina

#endif  // LUMINA_DB_IMAGE_DAO_H_
