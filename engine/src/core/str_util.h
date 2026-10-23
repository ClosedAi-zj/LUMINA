// 路径规范化 / 文件 IO / UTF-8↔UTF-16 / 时间格式化 / 卷信息（Win32 API 封装）
#ifndef LUMINA_CORE_STR_UTIL_H_
#define LUMINA_CORE_STR_UTIL_H_

#include <cstdint>
#include <string>
#include <vector>

namespace lumina {

// 目录项：名字（不含路径）、是否目录、字节大小、修改时间（epoch ms）
struct DirEntry {
  std::string name;
  bool isDir = false;
  int64_t size = 0;
  int64_t mtimeMs = 0;
};

// 卷信息（导入向导 SD 卡检测）
struct VolumeInfo {
  std::string path;    // "D:/"
  std::string label;   // 卷标
  uint64_t totalBytes = 0;
  uint64_t freeBytes = 0;
  bool isRemovable = false;
};

// ---- 路径 ----
std::string normalizePath(const std::string& path);      // 反斜杠转正斜杠、去尾斜杠
std::string joinPath(const std::string& a, const std::string& b);
std::string parentPath(const std::string& path);
std::string baseName(const std::string& path);           // 含扩展名
std::string baseNameNoExt(const std::string& path);
std::string extensionOf(const std::string& path);        // 小写、不含点
std::string replaceExtension(const std::string& path, const std::string& ext);
std::string toLowerAscii(const std::string& s);
std::string toUpperAscii(const std::string& s);
bool endsWith(const std::string& s, const std::string& suffix);
bool startsWith(const std::string& s, const std::string& prefix);
std::string trim(const std::string& s);
std::string padNumber(int value, int width);             // 4 位序号

// ---- 文件系统 ----
bool fileExists(const std::string& path);
bool dirExists(const std::string& path);
bool makeDirs(const std::string& path);
int64_t fileSize(const std::string& path);
int64_t fileMtimeMs(const std::string& path);
bool readFile(const std::string& path, std::string* out, std::string* err);
bool writeFileBytes(const std::string& path, const std::string& data, std::string* err);
// 原子写：同目录 temp 文件 → MoveFileEx(replace)，崩溃也不会留半截文件
bool writeFileAtomic(const std::string& path, const std::string& data, std::string* err);
bool removeFile(const std::string& path);
bool renamePath(const std::string& from, const std::string& to, std::string* err);
bool copyFileTo(const std::string& from, const std::string& to, std::string* err);
bool listDir(const std::string& path, std::vector<DirEntry>* out, std::string* err);
uint64_t freeBytesOf(const std::string& path, uint64_t* totalBytes);
std::vector<VolumeInfo> listVolumes();

// ---- 时间 ----
int64_t nowMs();
std::string formatTimestamp(int64_t ms);   // "YYYY-MM-DD HH:MM:SS.mmm"
std::string formatDate(int64_t ms);        // "YYYY-MM-DD"
std::string formatMonth(int64_t ms);       // "YYYY-MM"
std::string currentLogDate();              // "YYYYMMDD"

// ---- 编码 ----
std::wstring utf8ToUtf16(const std::string& s);
std::string utf16ToUtf8(const std::wstring& w);

// ---- XML ----
std::string xmlEscape(const std::string& s);
std::string xmlUnescape(const std::string& s);

// ---- 杂项 ----
std::string randomHex(size_t len);
std::string urlEncode(const std::string& s);

}  // namespace lumina

#endif  // LUMINA_CORE_STR_UTIL_H_
