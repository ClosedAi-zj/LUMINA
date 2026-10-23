// 导入前扫描：枚举源、格式统计、RAW+JPG 配对、重复与不可读清单
#ifndef LUMINA_IMPORT_SCANNER_H_
#define LUMINA_IMPORT_SCANNER_H_

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace lumina {
namespace import {

struct ScanItem {
  std::string path;
  std::string fileName;
  std::string format;
  int64_t size = 0;
  int64_t mtimeMs = 0;
  bool isRaw = false;
  bool isJpg = false;
  bool isVideo = false;
  std::string pairKey;  // 同名（去扩展名）配对键
};

struct ScanResult {
  int64_t totalFiles = 0;
  int64_t totalBytes = 0;
  std::map<std::string, int64_t> byFormat;
  int duplicates = 0;
  int rawWithJpg = 0;
  int jpgOnly = 0;
  std::vector<std::pair<std::string, std::string>> unreadable;  // (path, error)
  std::vector<ScanItem> items;
};

class ImportScanner {
 public:
  using ProgressFn = std::function<void(int percent, const std::string& detail)>;
  using CancelFn = std::function<bool()>;

  // 扫描目录（递归）。读失败计入 unreadable，不中断。
  static bool scanFolder(const std::string& rootPath, ScanResult* out, const ProgressFn& progress,
                         const CancelFn& cancelled, std::string* err);
};

}  // namespace import
}  // namespace lumina

#endif  // LUMINA_IMPORT_SCANNER_H_
