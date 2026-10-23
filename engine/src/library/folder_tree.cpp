// 文件夹树构建实现
#include "library/folder_tree.h"

#include "core/str_util.h"

namespace lumina {
namespace library {

std::vector<FolderNode> buildFolderTree(db::PhotoDao* photos, std::string* err) {
  std::vector<FolderNode> out;
  std::string dbErr;
  std::vector<RootRecord> roots = photos->listRoots(&dbErr);
  std::vector<db::FolderInfo> folders = photos->listFolders(&dbErr);
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
