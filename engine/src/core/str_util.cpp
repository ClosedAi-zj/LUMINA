// 路径规范化 / 文件 IO / UTF-8↔UTF-16 / 时间格式化 / 卷信息（Win32 API 封装实现）
#include "core/str_util.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstring>

namespace lumina {
namespace {

int64_t fileTimeToMs(const FILETIME& ft) {
  ULARGE_INTEGER ul;
  ul.LowPart = ft.dwLowDateTime;
  ul.HighPart = ft.dwHighDateTime;
  // Windows FILETIME 单位 100ns，起点 1601-01-01；转 Unix epoch ms
  uint64_t v = ul.QuadPart / 10000ULL;
  return static_cast<int64_t>(v) - 11644473600000LL;
}

}  // namespace

// ---------------- 路径 ----------------

std::string normalizePath(const std::string& path) {
  std::string out;
  out.reserve(path.size());
  for (char c : path) out.push_back(c == '\\' ? '/' : c);
  while (out.size() > 1 && out.back() == '/') out.pop_back();
  return out;
}

std::string joinPath(const std::string& a, const std::string& b) {
  if (b.empty()) return a;
  if (b.size() >= 2 && b[1] == ':') return b;  // b 本身是绝对路径
  if (b[0] == '/' || b[0] == '\\') return b;
  std::string out = a;
  if (!out.empty() && out.back() != '/' && out.back() != '\\') out.push_back('/');
  out += b;
  return out;
}

std::string parentPath(const std::string& path) {
  size_t pos = path.find_last_of("/\\");
  if (pos == std::string::npos) return std::string();
  if (pos == 0) return "/";
  return path.substr(0, pos);
}

std::string baseName(const std::string& path) {
  size_t pos = path.find_last_of("/\\");
  return pos == std::string::npos ? path : path.substr(pos + 1);
}

std::string baseNameNoExt(const std::string& path) {
  std::string name = baseName(path);
  size_t pos = name.find_last_of('.');
  return pos == std::string::npos ? name : name.substr(0, pos);
}

std::string extensionOf(const std::string& path) {
  std::string name = baseName(path);
  size_t pos = name.find_last_of('.');
  if (pos == std::string::npos) return std::string();
  return toLowerAscii(name.substr(pos + 1));
}

std::string replaceExtension(const std::string& path, const std::string& ext) {
  std::string out = path;
  size_t dot = out.find_last_of('.');
  size_t slash = out.find_last_of("/\\");
  if (dot != std::string::npos && (slash == std::string::npos || dot > slash)) {
    out.erase(dot);
  }
  if (!ext.empty()) {
    if (ext[0] != '.') out.push_back('.');
    out += ext;
  }
  return out;
}

std::string toLowerAscii(const std::string& s) {
  std::string out = s;
  for (char& c : out) {
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  }
  return out;
}

std::string toUpperAscii(const std::string& s) {
  std::string out = s;
  for (char& c : out) {
    if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
  }
  return out;
}

bool endsWith(const std::string& s, const std::string& suffix) {
  if (suffix.size() > s.size()) return false;
  return s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

bool startsWith(const std::string& s, const std::string& prefix) {
  if (prefix.size() > s.size()) return false;
  return s.compare(0, prefix.size(), prefix) == 0;
}

std::string trim(const std::string& s) {
  size_t b = 0;
  size_t e = s.size();
  while (b < e && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r' || s[b] == '\n')) ++b;
  while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r' || s[e - 1] == '\n')) --e;
  return s.substr(b, e - b);
}

std::string padNumber(int value, int width) {
  char buf[32];
  snprintf(buf, sizeof(buf), "%0*d", width, value);
  return std::string(buf);
}

// ---------------- 文件系统 ----------------

bool fileExists(const std::string& path) {
  if (path.empty()) return false;
  std::wstring w = utf8ToUtf16(path);
  DWORD attr = GetFileAttributesW(w.c_str());
  if (attr == INVALID_FILE_ATTRIBUTES) return false;
  return (attr & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

bool dirExists(const std::string& path) {
  if (path.empty()) return false;
  std::wstring w = utf8ToUtf16(path);
  DWORD attr = GetFileAttributesW(w.c_str());
  if (attr == INVALID_FILE_ATTRIBUTES) return false;
  return (attr & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

bool makeDirs(const std::string& path) {
  if (path.empty()) return false;
  if (dirExists(path)) return true;
  std::string parent = parentPath(path);
  if (!parent.empty() && !dirExists(parent)) {
    if (!makeDirs(parent)) return false;
  }
  std::wstring w = utf8ToUtf16(path);
  return CreateDirectoryW(w.c_str(), nullptr) != 0 || dirExists(path);
}

int64_t fileSize(const std::string& path) {
  std::wstring w = utf8ToUtf16(path);
  WIN32_FILE_ATTRIBUTE_DATA data;
  if (!GetFileAttributesExW(w.c_str(), GetFileExInfoStandard, &data)) return -1;
  if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) return -1;
  ULARGE_INTEGER ul;
  ul.HighPart = data.nFileSizeHigh;
  ul.LowPart = data.nFileSizeLow;
  return static_cast<int64_t>(ul.QuadPart);
}

int64_t fileMtimeMs(const std::string& path) {
  std::wstring w = utf8ToUtf16(path);
  WIN32_FILE_ATTRIBUTE_DATA data;
  if (!GetFileAttributesExW(w.c_str(), GetFileExInfoStandard, &data)) return -1;
  return fileTimeToMs(data.ftLastWriteTime);
}

bool readFile(const std::string& path, std::string* out, std::string* err) {
  std::wstring w = utf8ToUtf16(path);
  HANDLE h = CreateFileW(w.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                         FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) {
    if (err) *err = "open failed: " + path;
    return false;
  }
  LARGE_INTEGER li;
  if (!GetFileSizeEx(h, &li)) {
    CloseHandle(h);
    if (err) *err = "size failed: " + path;
    return false;
  }
  out->clear();
  if (li.QuadPart > 0) {
    out->resize(static_cast<size_t>(li.QuadPart));
    DWORD read = 0;
    BOOL ok = ReadFile(h, &(*out)[0], static_cast<DWORD>(out->size()), &read, nullptr);
    if (!ok || read != out->size()) {
      CloseHandle(h);
      if (err) *err = "read incomplete: " + path;
      return false;
    }
  }
  CloseHandle(h);
  return true;
}

bool writeFileBytes(const std::string& path, const std::string& data, std::string* err) {
  std::string parent = parentPath(path);
  if (!parent.empty() && !dirExists(parent) && !makeDirs(parent)) {
    if (err) *err = "mkdir failed: " + parent;
    return false;
  }
  std::wstring w = utf8ToUtf16(path);
  HANDLE h = CreateFileW(w.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                         FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) {
    if (err) *err = "create failed: " + path;
    return false;
  }
  DWORD written = 0;
  BOOL ok = TRUE;
  if (!data.empty()) {
    ok = WriteFile(h, data.data(), static_cast<DWORD>(data.size()), &written, nullptr);
  }
  CloseHandle(h);
  if (!ok || written != data.size()) {
    if (err) *err = "write failed: " + path;
    return false;
  }
  return true;
}

bool writeFileAtomic(const std::string& path, const std::string& data, std::string* err) {
  std::string tmp = path + ".tmp-" + randomHex(8);
  if (!writeFileBytes(tmp, data, err)) {
    removeFile(tmp);
    return false;
  }
  std::wstring wFrom = utf8ToUtf16(tmp);
  std::wstring wTo = utf8ToUtf16(path);
  if (!MoveFileExW(wFrom.c_str(), wTo.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    if (err) *err = "rename failed: " + tmp + " -> " + path;
    removeFile(tmp);
    return false;
  }
  return true;
}

bool removeFile(const std::string& path) {
  std::wstring w = utf8ToUtf16(path);
  return DeleteFileW(w.c_str()) != 0;
}

bool renamePath(const std::string& from, const std::string& to, std::string* err) {
  std::wstring wFrom = utf8ToUtf16(from);
  std::wstring wTo = utf8ToUtf16(to);
  if (!MoveFileExW(wFrom.c_str(), wTo.c_str(), MOVEFILE_REPLACE_EXISTING)) {
    if (err) *err = "rename failed: " + from + " -> " + to;
    return false;
  }
  return true;
}

bool copyFileTo(const std::string& from, const std::string& to, std::string* err) {
  std::string parent = parentPath(to);
  if (!parent.empty() && !dirExists(parent) && !makeDirs(parent)) {
    if (err) *err = "mkdir failed: " + parent;
    return false;
  }
  std::wstring wFrom = utf8ToUtf16(from);
  std::wstring wTo = utf8ToUtf16(to);
  if (!CopyFileW(wFrom.c_str(), wTo.c_str(), FALSE)) {
    if (err) *err = "copy failed: " + from + " -> " + to;
    return false;
  }
  return true;
}

bool listDir(const std::string& path, std::vector<DirEntry>* out, std::string* err) {
  out->clear();
  std::wstring w = utf8ToUtf16(path);
  WIN32_FIND_DATAW fd;
  std::wstring pattern = w + L"/*";
  HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
  if (h == INVALID_HANDLE_VALUE) {
    if (err) *err = "listDir failed: " + path;
    return false;
  }
  do {
    std::string name = utf16ToUtf8(fd.cFileName);
    if (name == "." || name == "..") continue;
    DirEntry entry;
    entry.name = name;
    entry.isDir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    ULARGE_INTEGER ul;
    ul.HighPart = fd.nFileSizeHigh;
    ul.LowPart = fd.nFileSizeLow;
    entry.size = static_cast<int64_t>(ul.QuadPart);
    entry.mtimeMs = fileTimeToMs(fd.ftLastWriteTime);
    out->push_back(entry);
  } while (FindNextFileW(h, &fd));
  FindClose(h);
  return true;
}

uint64_t freeBytesOf(const std::string& path, uint64_t* totalBytes) {
  std::wstring w = utf8ToUtf16(path.empty() ? std::string("C:/") : path);
  ULARGE_INTEGER freeBytes, total, totalFree;
  if (!GetDiskFreeSpaceExW(w.c_str(), &freeBytes, &total, &totalFree)) {
    if (totalBytes) *totalBytes = 0;
    return 0;
  }
  if (totalBytes) *totalBytes = total.QuadPart;
  return freeBytes.QuadPart;
}

std::vector<VolumeInfo> listVolumes() {
  std::vector<VolumeInfo> out;
  wchar_t drives[512];
  DWORD len = GetLogicalDriveStringsW(511, drives);
  if (len == 0 || len > 511) return out;
  wchar_t* p = drives;
  while (*p) {
    std::wstring root(p);
    UINT type = GetDriveTypeW(root.c_str());
    if (type == DRIVE_FIXED || type == DRIVE_REMOVABLE || type == DRIVE_REMOTE ||
        type == DRIVE_CDROM) {
      VolumeInfo info;
      info.path = utf16ToUtf8(root);
      info.isRemovable = (type == DRIVE_REMOVABLE || type == DRIVE_CDROM);
      wchar_t label[256] = {0};
      DWORD serial = 0, maxLen = 0, flags = 0;
      if (GetVolumeInformationW(root.c_str(), label, 255, &serial, &maxLen, &flags, nullptr, 0)) {
        info.label = utf16ToUtf8(label);
      }
      uint64_t total = 0;
      info.freeBytes = freeBytesOf(info.path, &total);
      info.totalBytes = total;
      if (info.totalBytes > 0) out.push_back(info);
    }
    p += wcslen(p) + 1;
  }
  return out;
}

// ---------------- 时间 ----------------

int64_t nowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

std::string formatTimestamp(int64_t ms) {
  time_t sec = static_cast<time_t>(ms / 1000);
  int msec = static_cast<int>(ms % 1000);
  if (msec < 0) msec += 1000;
  struct tm tmBuf;
  localtime_s(&tmBuf, &sec);
  char buf[64];
  snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d.%03d", tmBuf.tm_year + 1900,
           tmBuf.tm_mon + 1, tmBuf.tm_mday, tmBuf.tm_hour, tmBuf.tm_min, tmBuf.tm_sec, msec);
  return std::string(buf);
}

std::string formatDate(int64_t ms) { return formatTimestamp(ms).substr(0, 10); }

std::string formatMonth(int64_t ms) { return formatTimestamp(ms).substr(0, 7); }

std::string currentLogDate() {
  std::string d = formatDate(nowMs());
  std::string out;
  for (char c : d) {
    if (c != '-') out.push_back(c);
  }
  return out;
}

// ---------------- 编码 ----------------

std::wstring utf8ToUtf16(const std::string& s) {
  if (s.empty()) return std::wstring();
  int need = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
  if (need <= 0) return std::wstring();
  std::wstring out(static_cast<size_t>(need), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), &out[0], need);
  return out;
}

std::string utf16ToUtf8(const std::wstring& w) {
  if (w.empty()) return std::string();
  int need = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0,
                                 nullptr, nullptr);
  if (need <= 0) return std::string();
  std::string out(static_cast<size_t>(need), '\0');
  WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), &out[0], need, nullptr,
                      nullptr);
  return out;
}

// ---------------- XML ----------------

std::string xmlEscape(const std::string& s) {
  std::string out;
  out.reserve(s.size() + 8);
  for (char c : s) {
    switch (c) {
      case '&': out += "&amp;"; break;
      case '<': out += "&lt;"; break;
      case '>': out += "&gt;"; break;
      case '"': out += "&quot;"; break;
      case '\'': out += "&apos;"; break;
      default: out.push_back(c);
    }
  }
  return out;
}

std::string xmlUnescape(const std::string& s) {
  std::string out;
  out.reserve(s.size());
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] != '&') { out.push_back(s[i]); continue; }
    size_t end = s.find(';', i);
    if (end == std::string::npos || end - i > 8) { out.push_back(s[i]); continue; }
    std::string ent = s.substr(i + 1, end - i - 1);
    if (ent == "amp") out.push_back('&');
    else if (ent == "lt") out.push_back('<');
    else if (ent == "gt") out.push_back('>');
    else if (ent == "quot") out.push_back('"');
    else if (ent == "apos") out.push_back('\'');
    else { out.push_back(s[i]); continue; }
    i = end;
  }
  return out;
}

// ---------------- 杂项 ----------------

std::string randomHex(size_t len) {
  static const char* kHex = "0123456789abcdef";
  // 原子状态：旧实现是 `static uint64_t seed`，被 writeFileAtomic 的临时文件名
  // 多线程并发调用时是数据竞争（UB），且可能两条线程读到同一 seed 值产出完全相同的
  // 「随机」串 —— 临时文件名撞车会导致原子写互相覆盖。
  // 现在：xorshift64* + 原子 CAS，种子混入高精度时钟与进程内计数器。
  static std::atomic<uint64_t> state{0};
  uint64_t cur = state.load(std::memory_order_relaxed);
  if (cur == 0) {
    // 首次使用：用时间 + 计数器初始化（避免固定种子在跨进程时产生同一序列）
    static std::atomic<uint64_t> initCounter{0};
    const uint64_t t = static_cast<uint64_t>(
        std::chrono::high_resolution_clock::now().time_since_epoch().count());
    uint64_t seed = t ^ (initCounter.fetch_add(1) * 0x9E3779B97F4A7C15ULL);
    if (seed == 0) seed = 0x9E3779B97F4A7C15ULL;
    uint64_t expected = 0;
    state.compare_exchange_strong(expected, seed, std::memory_order_relaxed);
    cur = state.load(std::memory_order_relaxed);
  }
  std::string out;
  out.reserve(len);
  for (size_t i = 0; i < len; ++i) {
    // 每个字符取一个新的原子步进值：CAS 失败即重试，保证任意两个并发调用者
    // 永远不会拿到同一个中间状态。
    uint64_t prev = state.load(std::memory_order_relaxed);
    uint64_t next = prev;
    do {
      next = prev;
      next ^= next >> 12;
      next ^= next << 25;
      next ^= next >> 27;
      next *= 2685821657736338717ULL;
      if (next == 0) next = 0x9E3779B97F4A7C15ULL;
    } while (!state.compare_exchange_weak(prev, next, std::memory_order_relaxed));
    out.push_back(kHex[(next >> 33) & 0xF]);
  }
  return out;
}

std::string urlEncode(const std::string& s) {
  static const char* kHex = "0123456789ABCDEF";
  std::string out;
  for (unsigned char c : s) {
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' ||
        c == '_' || c == '.' || c == '~') {
      out.push_back(static_cast<char>(c));
    } else {
      out.push_back('%');
      out.push_back(kHex[(c >> 4) & 0xF]);
      out.push_back(kHex[c & 0xF]);
    }
  }
  return out;
}

}  // namespace lumina
