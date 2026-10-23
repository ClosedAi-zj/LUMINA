// 智能收藏夹预设规则实现
#include "library/smart_collections.h"

namespace lumina {
namespace library {

std::vector<SmartCollection> SmartCollections::all() {
  std::vector<SmartCollection> out;

  {
    SmartCollection c;
    c.id = "recent-edits";
    c.name = "最近调整";
    c.filter.edited = true;
    out.push_back(c);
  }
  {
    SmartCollection c;
    c.id = "top-rated";
    c.name = "高评分";
    c.filter.minRating = 4;
    out.push_back(c);
  }
  {
    SmartCollection c;
    c.id = "picks";
    c.name = "留底（Pick）";
    c.filter.pick = std::string("pick");
    out.push_back(c);
  }
  {
    SmartCollection c;
    c.id = "missing-files";
    c.name = "离线 / 缺失原片";
    c.filter.missing = true;
    out.push_back(c);
  }
  {
    SmartCollection c;
    c.id = "located";
    c.name = "带 GPS 定位";
    c.filter.located = true;
    out.push_back(c);
  }
  {
    SmartCollection c;
    c.id = "unlocated";
    c.name = "待补定位";
    c.filter.located = false;
    out.push_back(c);
  }
  {
    SmartCollection c;
    c.id = "raw-only";
    c.name = "仅 RAW";
    c.filter.formats = std::vector<std::string>{"RW2", "DNG", "ARW", "CR2", "CR3", "NEF"};
    out.push_back(c);
  }
  {
    SmartCollection c;
    c.id = "videos";
    c.name = "视频片段";
    c.filter.isVideo = true;
    out.push_back(c);
  }
  return out;
}

bool SmartCollections::find(const std::string& id, SmartCollection* out) {
  for (const SmartCollection& c : all()) {
    if (c.id == id) {
      *out = c;
      return true;
    }
  }
  return false;
}

}  // namespace library
}  // namespace lumina
