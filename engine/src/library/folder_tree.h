// 文件夹树：库根 → 相对路径折叠树，含每夹张数与缺失红点
#ifndef LUMINA_LIBRARY_FOLDER_TREE_H_
#define LUMINA_LIBRARY_FOLDER_TREE_H_

#include <string>
#include <vector>

#include "core/models.h"
#include "db/database.h"
#include "db/photo_dao.h"

namespace lumina {
namespace library {

// 从 photos / folders 聚合出树；目录在磁盘上不存在时标记 missing
std::vector<FolderNode> buildFolderTree(db::PhotoDao* photos, std::string* err);

}  // namespace library
}  // namespace lumina

#endif  // LUMINA_LIBRARY_FOLDER_TREE_H_
