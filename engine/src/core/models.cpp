// 共享数据结构 ↔ JSON 转换实现（字段名严格对齐 docs/ipc-protocol.md §4）
#include "core/models.h"

namespace lumina {

bool Filter::empty() const {
  return !folderIds && !rootIds && !formats && !cameras && !lenses && !minRating && !pick &&
         !located && !edited && !hasMasks && !missing && !isVideo && !dateFrom && !dateTo &&
         !locationIds && !seqId && !colorLabel && !maxRating;
}

namespace {

void pushStringArray(const std::vector<std::string>& in, json::JsonValue* out) {
  for (const std::string& s : in) out->addItem(s);
}

void pushIntArray(const std::vector<int64_t>& in, json::JsonValue* out) {
  for (int64_t v : in) out->addItem(v);
}

}  // namespace

json::JsonValue toPhotoSummary(const PhotoRecord& p) {
  json::JsonValue j = json::JsonValue::objectValue();
  j.setItem("id", p.id);
  j.setItem("fileName", p.fileName);
  j.setItem("filePath", p.filePath);
  j.setItem("format", p.format);
  j.setItem("width", p.width);
  j.setItem("height", p.height);
  j.setItem("capturedAt", p.capturedAt);
  j.setItem("importedAt", p.importedAt);
  j.setItem("camera", p.camera);
  j.setItem("lens", p.lens);
  j.setItem("iso", p.iso);
  j.setItem("aperture", p.aperture);
  j.setItem("shutter", p.shutter);
  j.setItem("focalLength", p.focalLength);
  if (p.hasGps) {
    j.setItem("gpsLat", p.gpsLat);
    j.setItem("gpsLng", p.gpsLng);
  } else {
    j.setItem("gpsLat", json::JsonValue::nullValue());
    j.setItem("gpsLng", json::JsonValue::nullValue());
  }
  j.setItem("rating", p.rating);
  j.setItem("pick", p.pick);
  j.setItem("colorLabel", p.colorLabel.empty() ? json::JsonValue::nullValue()
                                              : json::JsonValue::str(p.colorLabel));
  j.setItem("hasEdits", p.hasEdits);
  j.setItem("hasGps", p.hasGps);
  j.setItem("isMissing", p.isMissing);
  j.setItem("isVideo", p.isVideo);
  j.setItem("pairKind", p.pairKind);
  j.setItem("thumbState", p.thumbState);
  j.setItem("thumbPath", p.thumbPath.empty() ? json::JsonValue::nullValue()
                                             : json::JsonValue::str(p.thumbPath));
  j.setItem("seqId", p.hasSeqId ? json::JsonValue::integer(p.seqId) : json::JsonValue::nullValue());
  return j;
}

json::JsonValue toPhotoDetail(const PhotoRecord& p) {
  json::JsonValue j = toPhotoSummary(p);
  j.setItem("md5", p.md5.empty() ? json::JsonValue::nullValue() : json::JsonValue::str(p.md5));
  j.setItem("fileSize", p.fileSize);
  j.setItem("sidecarPath", p.sidecarPath.empty() ? json::JsonValue::nullValue()
                                                 : json::JsonValue::str(p.sidecarPath));
  j.setItem("sidecarMtime", p.hasSidecarMtime ? json::JsonValue::integer(p.sidecarMtime)
                                              : json::JsonValue::nullValue());
  j.setItem("whiteBalance", p.whiteBalance.empty() ? json::JsonValue::str("asShot")
                                                   : json::JsonValue::str(p.whiteBalance));
  j.setItem("folderId", p.folderId);
  j.setItem("rootId", p.rootId);
  json::JsonValue keywords = json::JsonValue::arrayValue();
  if (!p.keywords.empty()) {
    // keywords 以 ';' 分隔存储
    std::string cur;
    for (char c : p.keywords) {
      if (c == ';' || c == ',') {
        if (!cur.empty()) keywords.addItem(cur);
        cur.clear();
      } else {
        cur.push_back(c);
      }
    }
    if (!cur.empty()) keywords.addItem(cur);
  }
  j.setItem("keywords", keywords);
  j.setItem("description", p.description);
  j.setItem("seqInfo", json::JsonValue::nullValue());
  j.setItem("health", p.health);
  json::JsonValue backup = json::JsonValue::objectValue();
  backup.setItem("backedUp", p.backedUp);
  backup.setItem("backupPath", p.backupPath.empty() ? json::JsonValue::nullValue()
                                                    : json::JsonValue::str(p.backupPath));
  backup.setItem("verifiedAt", p.hasBackupVerifiedAt ? json::JsonValue::integer(p.backupVerifiedAt)
                                                     : json::JsonValue::nullValue());
  j.setItem("backup", backup);
  return j;
}

json::JsonValue toQueryResult(const QueryResult& r) {
  json::JsonValue j = json::JsonValue::objectValue();
  j.setItem("total", r.total);
  json::JsonValue items = json::JsonValue::arrayValue();
  for (const PhotoRecord& p : r.items) items.addItem(toPhotoSummary(p));
  j.setItem("items", items);
  return j;
}

bool filterFromJson(const json::JsonValue& v, Filter* out, std::string* err) {
  if (!v.isNull() && !v.isObject()) {
    if (err) *err = "filter must be an object";
    return false;
  }
  Filter f;
  auto readInts = [&v](const char* key, std::optional<std::vector<int64_t>>* target) {
    const json::JsonValue& arr = v.get(key);
    if (!arr.isArray()) return;
    std::vector<int64_t> list;
    for (const auto& item : arr.arrayItems()) list.push_back(item.asInt());
    *target = list;
  };
  auto readStrings = [&v](const char* key, std::optional<std::vector<std::string>>* target) {
    const json::JsonValue& arr = v.get(key);
    if (!arr.isArray()) return;
    std::vector<std::string> list;
    for (const auto& item : arr.arrayItems()) list.push_back(item.asString());
    *target = list;
  };
  readInts("folderIds", &f.folderIds);
  readInts("rootIds", &f.rootIds);
  readStrings("formats", &f.formats);
  readStrings("cameras", &f.cameras);
  readStrings("lenses", &f.lenses);
  readInts("locationIds", &f.locationIds);
  if (v.contains("minRating")) f.minRating = static_cast<int>(v.get("minRating").asInt());
  if (v.contains("maxRating")) f.maxRating = static_cast<int>(v.get("maxRating").asInt());
  if (v.contains("pick")) f.pick = v.get("pick").asString();
  if (v.contains("located")) f.located = v.get("located").asBool();
  if (v.contains("edited")) f.edited = v.get("edited").asBool();
  if (v.contains("hasMasks")) f.hasMasks = v.get("hasMasks").asBool();
  if (v.contains("missing")) f.missing = v.get("missing").asBool();
  if (v.contains("isVideo")) f.isVideo = v.get("isVideo").asBool();
  if (v.contains("dateFrom")) f.dateFrom = v.get("dateFrom").asInt();
  if (v.contains("dateTo")) f.dateTo = v.get("dateTo").asInt();
  if (v.contains("seqId")) f.seqId = v.get("seqId").asInt();
  if (v.contains("colorLabel")) f.colorLabel = v.get("colorLabel").asString();
  *out = f;
  return true;
}

json::JsonValue filterToJson(const Filter& f) {
  json::JsonValue j = json::JsonValue::objectValue();
  if (f.folderIds) {
    json::JsonValue a = json::JsonValue::arrayValue();
    pushIntArray(*f.folderIds, &a);
    j.setItem("folderIds", a);
  }
  if (f.rootIds) {
    json::JsonValue a = json::JsonValue::arrayValue();
    pushIntArray(*f.rootIds, &a);
    j.setItem("rootIds", a);
  }
  if (f.formats) {
    json::JsonValue a = json::JsonValue::arrayValue();
    pushStringArray(*f.formats, &a);
    j.setItem("formats", a);
  }
  if (f.cameras) {
    json::JsonValue a = json::JsonValue::arrayValue();
    pushStringArray(*f.cameras, &a);
    j.setItem("cameras", a);
  }
  if (f.lenses) {
    json::JsonValue a = json::JsonValue::arrayValue();
    pushStringArray(*f.lenses, &a);
    j.setItem("lenses", a);
  }
  if (f.locationIds) {
    json::JsonValue a = json::JsonValue::arrayValue();
    pushIntArray(*f.locationIds, &a);
    j.setItem("locationIds", a);
  }
  if (f.minRating) j.setItem("minRating", *f.minRating);
  if (f.maxRating) j.setItem("maxRating", *f.maxRating);
  if (f.pick) j.setItem("pick", *f.pick);
  if (f.located) j.setItem("located", *f.located);
  if (f.edited) j.setItem("edited", *f.edited);
  if (f.hasMasks) j.setItem("hasMasks", *f.hasMasks);
  if (f.missing) j.setItem("missing", *f.missing);
  if (f.isVideo) j.setItem("isVideo", *f.isVideo);
  if (f.dateFrom) j.setItem("dateFrom", *f.dateFrom);
  if (f.dateTo) j.setItem("dateTo", *f.dateTo);
  if (f.seqId) j.setItem("seqId", *f.seqId);
  if (f.colorLabel) j.setItem("colorLabel", *f.colorLabel);
  return j;
}

bool sortFromJson(const json::JsonValue& v, Sort* out) {
  Sort s;
  if (v.isObject()) {
    std::string field = v.get("field").asString("capturedAt");
    if (field == "importedAt" || field == "fileName" || field == "rating" ||
        field == "capturedAt") {
      s.field = field;
    }
    s.desc = v.get("order").asString("asc") == "desc";
  }
  *out = s;
  return true;
}

bool pageFromJson(const json::JsonValue& v, Page* out) {
  Page p;
  if (v.isObject()) {
    p.offset = static_cast<int>(v.get("offset").asInt(0));
    p.limit = static_cast<int>(v.get("limit").asInt(200));
  }
  if (p.offset < 0) p.offset = 0;
  if (p.limit <= 0) p.limit = 200;
  if (p.limit > 500) p.limit = 500;  // 协议 §4 Page：limit ≤ 500
  *out = p;
  return true;
}

json::JsonValue folderNodeToJson(const FolderNode& n) {
  json::JsonValue j = json::JsonValue::objectValue();
  j.setItem("id", n.id);
  j.setItem("rootId", n.rootId);
  j.setItem("path", n.path);
  j.setItem("name", n.name);
  j.setItem("count", n.count);
  j.setItem("missing", n.missing);
  json::JsonValue children = json::JsonValue::arrayValue();
  for (const FolderNode& c : n.children) children.addItem(folderNodeToJson(c));
  j.setItem("children", children);
  return j;
}

}  // namespace lumina
