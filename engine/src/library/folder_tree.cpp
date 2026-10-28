// 文件夹树构建实现
#include "library/folder_tree.h"

#include "core/str_util.h"

namespace lumina {
namespace library {

std::vector<FolderNode> buildFolderTree(db::PhotoDao* photos, std::string* err) {
  std::vector<FolderNode> out;
  if (!photos) {
    if (err) *err = "buildFolderTree: PhotoDao 未就绪";
    return out;
  }
  // 两次查询各用独立错误变量：共用同一个 dbErr 会让第二次覆盖第一次的错误，
  // 而末尾又从不检查它 —— DB 故障时返回空树，RPC 回 ok:true 空数组，
  // 用户看到的是「图库突然空了」（静默谎报）。
  std::string rootsErr;
  std::vector<RootRecord> roots = photos->listRoots(&rootsErr);
  if (!rootsErr.empty()) {
    if (err) *err = "buildFolderTree: 读取库根失败: " + rootsErr;
    return out;
  }
  std::string foldersErr;
  std::vector<db::FolderInfo> folders = photos->listFolders(&foldersErr);
  if (!foldersErr.empty()) {
    if (err) *err = "buildFolderTree: 读取文件夹失败: " + foldersErr;
    return out;
  }
  for (const RootRecord& root : roots) {
    FolderNode node;
    node.id = 0;
    node.rootId = root.id;
    node.path = root.path;
    node.name = root.label.empty() ? baseName(root.path) : root.label;
    node.missing = !dirExists(root.path);
    for (const db::FolderInfo& f : folders) {
      if (f.rootId != root.id) continue;
      FolderNode child;
      child.id = f.id;
      child.rootId = f.rootId;
      child.path = f.relPath;
      child.name = f.name;
      child.count = f.count;
      child.missing = f.missing;
      node.children.push_back(child);
      node.count += f.count;
    }
    out.push_back(node);
  }
  return out;
}

}  // namespace library
}  // namespace lumina
