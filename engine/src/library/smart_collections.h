// 智能收藏夹：≥6 条预设规则（PRD：一次点击直达高频筛选）
#ifndef LUMINA_LIBRARY_SMART_COLLECTIONS_H_
#define LUMINA_LIBRARY_SMART_COLLECTIONS_H_

#include <vector>

#include "core/models.h"

namespace lumina {
namespace library {

class SmartCollections {
 public:
  // 返回全部预设（id / name / filter）
  static std::vector<SmartCollection> all();
  // 按 id 查找
  static bool find(const std::string& id, SmartCollection* out);
};

}  // namespace library
}  // namespace lumina

#endif  // LUMINA_LIBRARY_SMART_COLLECTIONS_H_
