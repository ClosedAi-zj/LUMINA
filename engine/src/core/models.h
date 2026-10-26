// 跨模块共享数据结构（DB 行 / 查询条件 / 结果），以及 ↔ JSON 的序列化
#ifndef LUMINA_CORE_MODELS_H_
#define LUMINA_CORE_MODELS_H_

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "json/json_value.h"

namespace lumina {

// photos 表一行（完整）
struct PhotoRecord {
  int64_t id = 0;
  int64_t folderId = 0;
  int64_t rootId = 0;
  std::string fileName;
  std::string filePath;
  std::string format;
  int64_t fileSize = 0;
  std::string md5;
  int width = 0;
  int height = 0;
  int64_t capturedAt = 0;
  int64_t importedAt = 0;
  std::string camera;
  std::string lens;
  int iso = 0;
  std::string aperture;
  std::string shutter;
  std::string focalLength;
  std::string whiteBalance;
  double gpsLat = 0.0;
  double gpsLng = 0.0;
  double gpsAlt = 0.0;
  bool hasGps = false;
  int64_t locationId = 0;
  bool hasLocationId = false;
  int rating = 0;
  std::string pick = "none";
  std::string colorLabel;
  std::string keywords;
  std::string description;
  std::string sidecarPath;
  int64_t sidecarMtime = 0;
  bool hasSidecarMtime = false;
  bool hasEdits = false;
  bool isMissing = false;
  bool isVideo = false;
  std::string pairKind = "single";
  std::string health = "unchecked";
  bool deleted = false;
  int64_t seqId = 0;
  bool hasSeqId = false;
  bool backedUp = false;
  std::string backupPath;
  int64_t backupVerifiedAt = 0;
  bool hasBackupVerifiedAt = false;
  std::string thumbState = "pending";
  std::string thumbPath;
  // FTS5 外部内容表列（photos.location / photos.date，与 schema.sql 同名列）：
  // location = 地点名（中文，供全文检索）；date = "YYYY-MM"（时间轴 / 统计分组键）
  std::string location;
  std::string date;
};

// 多维 AND 筛选（ipc-protocol §4 Filter）
struct Filter {
  std::optional<std::vector<int64_t>> folderIds;
  std::optional<std::vector<int64_t>> rootIds;
  std::optional<std::vector<std::string>> formats;
  std::optional<std::vector<std::string>> cameras;
  std::optional<std::vector<std::string>> lenses;
  std::optional<int> minRating;
  std::optional<std::string> pick;
  std::optional<bool> located;
  std::optional<bool> edited;
  // M2 T2.3：是否含局部蒙版（走 develop_settings 的部分索引 idx_dev_masks）
  std::optional<bool> hasMasks;
  std::optional<bool> missing;
  std::optional<bool> isVideo;
  std::optional<int64_t> dateFrom;
  std::optional<int64_t> dateTo;
  std::optional<std::vector<int64_t>> locationIds;
  std::optional<int64_t> seqId;
  std::optional<std::string> colorLabel;
  std::optional<int> maxRating;
  bool includeDeleted = false;

  bool empty() const;
};

struct Sort {
  std::string field = "capturedAt";  // capturedAt|importedAt|fileName|rating
  bool desc = false;                 // 默认 capturedAt asc（协议 §4 Sort）
};

struct Page {
  int offset = 0;
  int limit = 200;
};

struct QueryResult {
  int64_t total = 0;
  std::vector<PhotoRecord> items;
};

// 时间线月桶
struct MonthBucket {
  std::string month;  // "2026-04"
  int64_t count = 0;
  int64_t firstPhotoId = 0;
};

// 文件夹树节点
struct FolderNode {
  int64_t id = 0;
  int64_t rootId = 0;
  std::string path;
  std::string name;
  int64_t count = 0;
  bool missing = false;
  std::vector<FolderNode> children;
};

// 库根节点
struct RootRecord {
  int64_t id = 0;
  std::string path;
  std::string label;
};

// 健康异常
struct HealthIssue {
  int64_t issueId = 0;
  int64_t photoId = 0;
  std::string kind;  // missing|corrupt
  std::string path;
  std::string lastKnown;
  std::string status;  // open|resolved|orphan
  int64_t detectedAt = 0;
};

struct HealthCounts {
  int64_t verified = 0;
  int64_t total = 0;
  int64_t missing = 0;
  int64_t corrupt = 0;
  int64_t unchecked = 0;
};

// 相册
struct AlbumRecord {
  int64_t id = 0;
  std::string name;
  std::string kind;  // collection|portfolio
  int64_t itemCount = 0;
  int64_t coverPhotoId = 0;
  bool hasCover = false;
  std::string publishedToken;
  int64_t publishedAt = 0;
  bool hasPublishedAt = false;
  int64_t expiresAt = 0;
  bool hasExpiresAt = false;
};

// 智能收藏夹
struct SmartCollection {
  std::string id;
  std::string name;
  Filter filter;
};

// 地点
struct LocationRecord {
  int64_t id = 0;
  std::string name;
  double lat = 0.0;
  double lng = 0.0;
  double alt = 0.0;
  bool favorite = false;
  int64_t count = 0;
};

// ---- JSON 转换 ----
json::JsonValue toPhotoSummary(const PhotoRecord& p);
json::JsonValue toPhotoDetail(const PhotoRecord& p);
json::JsonValue toQueryResult(const QueryResult& r);
bool filterFromJson(const json::JsonValue& v, Filter* out, std::string* err);
json::JsonValue filterToJson(const Filter& f);
bool sortFromJson(const json::JsonValue& v, Sort* out);
bool pageFromJson(const json::JsonValue& v, Page* out);
json::JsonValue folderNodeToJson(const FolderNode& n);

}  // namespace lumina

#endif  // LUMINA_CORE_MODELS_H_
