// {home} 目录布局、settings.json 读写、库根目录管理实现
#include "core/config.h"

#include <cctype>
#include <cstdlib>
#include <fstream>

#include "core/logger.h"
#include "core/str_util.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace lumina {
namespace core {
namespace {

json::JsonValue defaultSettings() {
  json::JsonValue s = json::JsonValue::objectValue();
  s.setItem("theme", "dark");
  s.setItem("homeDir", std::string());
  s.setItem("libraryRoots", json::JsonValue::arrayValue());
  s.setItem("backupTarget", std::string());
  s.setItem("backupEnabled", false);
  s.setItem("backupTiming", "onImport");
  s.setItem("thumbCacheQuotaMB", 5120);
  // T0.7：缓存格式版本（v2 = 16bit log16 地基 + 三键缓存）。升级时旧 v1 缓存
  // 目录整体作废重建，禁止混读（格式不兼容时宁可重算，绝不静默降采样）。
  s.setItem("cacheFormatVersion", 2);
  s.setItem("recentSources", json::JsonValue::arrayValue());
  s.setItem("mockLibraryRoot", std::string());  // 由 init() 按 {home}/mocklib 填充，避免硬编码绝对路径
  return s;
}

}  // namespace

bool Config::init(const std::string& homeDir, std::string* err) {
  // 注意：本函数内**不得在持有 mutex_ 时调用** homeDir()/cacheDir()/settingsPath()/save()
  // 等会再次加锁的成员——同线程重入 std::mutex 会死锁（曾导致引擎启动后挂起、RPC 无响应）。
  // 因此先把所有路径算好并落盘，最后一次性提交内存状态。
  const std::string normalized = normalizePath(homeDir);
  if (normalized.empty()) {
    if (err) *err = "empty home dir";
    return false;
  }
  if (!makeDirs(normalized)) {
    if (err) *err = "cannot create home dir: " + normalized;
    return false;
  }

  const std::string cache = joinPath(normalized, "cache");
  if (!makeDirs(cache)) {
    if (err) *err = "cannot create cache dir: " + cache;
    return false;
  }
  makeDirs(joinPath(cache, "thumbs"));
  makeDirs(joinPath(cache, "previews"));
  makeDirs(joinPath(cache, "renders"));
  makeDirs(joinPath(normalized, "logs"));
  makeDirs(joinPath(normalized, "gallery"));
  makeDirs(joinPath(normalized, "backups"));  // WP-A2：DB 快照目录（保留 7 份）

  json::JsonValue settings = defaultSettings();
  settings.setItem("homeDir", normalized);
  const std::string settingsFile = joinPath(normalized, "settings.json");
  if (fileExists(settingsFile)) {
    std::string text;
    std::string loadErr;
    if (readFile(settingsFile, &text, &loadErr)) {
      json::JsonValue loaded;
      std::string parseErr;
      if (json::JsonValue::parse(text, &loaded, &parseErr) && loaded.isObject()) {
        settings.mergePatch(loaded);
      } else {
        logWarn("config", "settings.json 解析失败，使用默认值: " + parseErr);
      }
    }
  }
  settings.setItem("homeDir", normalized);
  const json::JsonValue mockRoot = settings.get("mockLibraryRoot");
  if (!mockRoot.isString() || mockRoot.stringRef().empty()) {
    settings.setItem("mockLibraryRoot", joinPath(normalized, "mocklib"));
  }

  std::string saveErr;
  if (!writeFileAtomic(settingsFile, settings.serialize(), &saveErr)) {
    logWarn("config", "settings.json 写入失败: " + saveErr);
  }

  std::lock_guard<std::mutex> lock(mutex_);
  homeDir_ = normalized;
  settings_ = settings;
  return true;
}

bool Config::load(std::string* err) {
  // 读盘时不持锁（避免 settingsPath() → homeDir() 重入加锁死锁）
  std::string path;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    path = joinPath(homeDir_, "settings.json");
  }
  std::string text;
  if (!readFile(path, &text, err)) return false;
  json::JsonValue loaded;
  std::string parseErr;
  if (!json::JsonValue::parse(text, &loaded, &parseErr) || !loaded.isObject()) {
    if (err) *err = "bad settings.json: " + parseErr;
    return false;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  settings_.mergePatch(loaded);
  return true;
}

bool Config::save(std::string* err) {
  // 同样避免持锁调用 settingsPath()（内部会调 homeDir() 再加锁）
  std::string text;
  std::string path;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    text = settings_.serialize();
    path = joinPath(homeDir_, "settings.json");
  }
  return writeFileAtomic(path, text, err);
}

std::string Config::homeDir() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return homeDir_;
}
std::string Config::dbPath() const { return joinPath(homeDir(), "lumina.db"); }
std::string Config::cacheDir() const { return joinPath(homeDir(), "cache"); }
std::string Config::thumbsDir() const { return joinPath(cacheDir(), "thumbs"); }
std::string Config::previewsDir() const { return joinPath(cacheDir(), "previews"); }
std::string Config::rendersDir() const { return joinPath(cacheDir(), "renders"); }
int Config::cacheFormatVersion() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return static_cast<int>(settings_.get("cacheFormatVersion").asInt(kCacheFormatVersion));
}
std::string Config::rendersVersionedDir() const {
  return joinPath(rendersDir(), "v" + std::to_string(cacheFormatVersion()));
}
std::string Config::logsDir() const { return joinPath(homeDir(), "logs"); }
std::string Config::galleryDir() const { return joinPath(homeDir(), "gallery"); }
std::string Config::backupsDir() const { return joinPath(homeDir(), "backups"); }
std::string Config::settingsPath() const { return joinPath(homeDir(), "settings.json"); }
std::string Config::lockPath() const { return joinPath(homeDir(), "engine.lock"); }

std::vector<std::string> Config::libraryRoots() const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<std::string> out;
  const json::JsonValue& roots = settings_.get("libraryRoots");
  if (roots.isArray()) {
    for (const auto& item : roots.arrayItems()) {
      out.push_back(normalizePath(item.asString()));
    }
  }
  return out;
}

void Config::setLibraryRoots(const std::vector<std::string>& roots) {
  std::lock_guard<std::mutex> lock(mutex_);
  json::JsonValue arr = json::JsonValue::arrayValue();
  for (const std::string& r : roots) arr.addItem(normalizePath(r));
  settings_.setItem("libraryRoots", arr);
}

std::string Config::backupTarget() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return settings_.get("backupTarget").asString();
}

void Config::setBackupTarget(const std::string& path) {
  std::lock_guard<std::mutex> lock(mutex_);
  settings_.setItem("backupTarget", normalizePath(path));
}

int Config::thumbCacheQuotaMB() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return static_cast<int>(settings_.get("thumbCacheQuotaMB").asInt(5120));
}

std::string Config::theme() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return settings_.get("theme").asString("dark");
}

json::JsonValue Config::settings() const {
  // 陷阱：dbPath()/thumbsDir()/previewsDir()/logsDir() 内部都会经 homeDir() 加 mutex_。
  // 若在持有 mutex_ 时调用它们，等于同线程重入非递归锁 → 未定义行为（Windows 上表现为
  // 直接崩进程且不留任何日志）。所以必须先算路径，最后再一次性加锁取 settings_ 快照。
  const std::string db = dbPath();
  const std::string thumbs = thumbsDir();
  const std::string previews = previewsDir();
  const std::string logs = logsDir();

  std::lock_guard<std::mutex> lock(mutex_);
  json::JsonValue copy = settings_;
  copy.setItem("dbPath", db);
  copy.setItem("thumbsDir", thumbs);
  copy.setItem("previewsDir", previews);
  copy.setItem("logsDir", logs);
  return copy;
}

bool Config::mergePatch(const json::JsonValue& patch, std::string* err) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    settings_.mergePatch(patch);
    settings_.setItem("homeDir", homeDir_);
  }
  return save(err);
}

std::vector<std::string> Config::recentSources() const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<std::string> out;
  const json::JsonValue& arr = settings_.get("recentSources");
  if (arr.isArray()) {
    for (const auto& item : arr.arrayItems()) out.push_back(item.asString());
  }
  return out;
}

void Config::pushRecentSource(const std::string& path) {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<std::string> list;
  const json::JsonValue& arr = settings_.get("recentSources");
  if (arr.isArray()) {
    for (const auto& item : arr.arrayItems()) {
      if (item.asString() != path) list.push_back(item.asString());
    }
  }
  list.insert(list.begin(), normalizePath(path));
  if (list.size() > 5) list.resize(5);
  json::JsonValue out = json::JsonValue::arrayValue();
  for (const std::string& s : list) out.addItem(s);
  settings_.setItem("recentSources", out);
}

namespace {

// 从锁文件内容里取出 pid（格式 "pid=N ts=N"）。取不到返回 0。
DWORD parseLockPid(const std::string& text) {
  const std::string key = "pid=";
  const std::string::size_type at = text.find(key);
  if (at == std::string::npos) return 0;
  std::string digits;
  for (std::string::size_type i = at + key.size(); i < text.size() && isdigit((unsigned char)text[i]); ++i) {
    digits.push_back(text[i]);
  }
  if (digits.empty()) return 0;
  return static_cast<DWORD>(strtoul(digits.c_str(), nullptr, 10));
}

// 进程是否还活着（只做查询，不等待）。
bool processAlive(DWORD pid) {
  if (pid == 0) return false;
  HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
  if (!p) {
    // ERROR_ACCESS_DENIED 也是「存在但不可查询」；其余（无效参数/不存在）按不存在。
    return GetLastError() == ERROR_ACCESS_DENIED;
  }
  DWORD code = 0;
  const bool still = GetExitCodeProcess(p, &code) && code == STILL_ACTIVE;
  CloseHandle(p);
  return still;
}

// 读现存锁文件内容。打不开返回 false（多半是持有者仍以独占方式开着，视为活锁）。
bool readLockFile(const std::string& path, std::string* text) {
  std::wstring w = utf8ToUtf16(path);
  HANDLE h = CreateFileW(w.c_str(), GENERIC_READ,
                         FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                         OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) return false;
  char buf[256] = {0};
  DWORD got = 0;
  const bool ok = ReadFile(h, buf, sizeof(buf) - 1, &got, nullptr) != FALSE;
  CloseHandle(h);
  if (!ok) return false;
  text->assign(buf, got);
  return true;
}

}  // namespace

bool Config::acquireLock(std::string* err) {
  std::string path = lockPath();
  std::wstring w = utf8ToUtf16(path);
  // 独占创建；已存在即说明同 home 有另一实例在跑（架构 §9-D8）。
  // 持有期间允许他人只读（FILE_SHARE_READ），这样别的进程能读出 pid 判断死活；
  // 不给 WRITE/DELETE 共享，避免活锁被别人删掉。
  // v1.4.1：补陈旧锁回收 —— 引擎被强杀/断电后 DELETE_ON_CLOSE 不一定生效，
  // 残留的 engine.lock 会让之后每一次启动都失败并降级 Mock，且用户无从自救。
  HANDLE h = INVALID_HANDLE_VALUE;
  for (int attempt = 0; attempt < 12 && h == INVALID_HANDLE_VALUE; ++attempt) {
    h = CreateFileW(w.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_NEW,
                    FILE_ATTRIBUTE_NORMAL | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    if (h != INVALID_HANDLE_VALUE) break;
    const DWORD e = GetLastError();
    if (e == ERROR_FILE_EXISTS || e == ERROR_ACCESS_DENIED) {
      // 文件存在，或处于「已关闭待删除」的中间态（上一个进程刚退出）。
      std::string text;
      if (readLockFile(path, &text)) {
        const DWORD pid = parseLockPid(text);
        if (pid != 0 && pid != GetCurrentProcessId() && processAlive(pid)) {
          if (err) {
            *err = "another engine instance is running for home: " + homeDir() + " (pid=" +
                   std::to_string(static_cast<unsigned long>(pid)) + ")";
          }
          return false;
        }
        // 陈旧锁（pid 已死 / 无 pid 字段）：删掉重来，留痕不静默。
        logWarn("config", "回收陈旧单实例锁 " + path + "（pid=" +
                              std::to_string(static_cast<unsigned long>(pid)) + " 已不存在）");
        DeleteFileW(w.c_str());
        continue;
      }
      // 读不到：持有者很可能仍以独占方式开着（旧版引擎），或正处于删除中间态。
      // 短暂等待后重试，等对方退出/系统完成删除。
      Sleep(150);
      continue;
    }
    // 其它错误（路径不存在、权限问题等）不必重试
    break;
  }
  if (h == INVALID_HANDLE_VALUE) {
    if (err) {
      *err = "another engine instance is running for home: " + homeDir() +
             " (lock busy, win32=" + std::to_string(static_cast<unsigned long>(GetLastError())) + ")";
    }
    return false;
  }
  lockHandle_ = h;
  lockFilePath_ = path;
  std::string content = "pid=" + std::to_string(static_cast<long long>(GetCurrentProcessId())) +
                        " ts=" + std::to_string(nowMs());
  DWORD written = 0;
  WriteFile(h, content.data(), static_cast<DWORD>(content.size()), &written, nullptr);
  FlushFileBuffers(h);
  return true;
}

void Config::releaseLock() {
  if (lockHandle_) {
    CloseHandle(lockHandle_);
    lockHandle_ = nullptr;
  }
}

}  // namespace core
}  // namespace lumina
