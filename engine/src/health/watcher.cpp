// 文件系统实时监测实现（每个库根一个 ReadDirectoryChangesW 线程）
#include "health/watcher.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <cstring>

#include <windows.h>

#include "core/logger.h"
#include "core/str_util.h"

namespace lumina {
namespace health {

HealthWatcher::HealthWatcher(core::Config* config, db::PhotoDao* photos, db::HealthDao* health,
                             NotifyFn notify)
    : config_(config), photos_(photos), health_(health), notify_(std::move(notify)),
      running_(false), stopCounter_(0) {}

HealthWatcher::~HealthWatcher() { stop(); }

bool HealthWatcher::start(std::string* err) {
  if (running_.exchange(true)) return true;
  std::vector<RootRecord> roots = photos_->listRoots(err);
  for (const RootRecord& root : roots) {
    if (!dirExists(root.path)) continue;
    threads_.emplace_back([this, root]() { this->watchRoot(root.id, root.path); });
  }
  if (threads_.empty()) {
    logWarn("health", "no library root available for fs watching");
  } else {
    logInfo("health", "fs watcher started on " + std::to_string(threads_.size()) + " roots");
  }
  return true;
}

void HealthWatcher::stop() {
  if (!running_.exchange(false)) return;
  stopCounter_.fetch_add(1);
  // 必须 join 而非 detach：detach 后 worker 仍可能访问 photos_/health_/notify_（它们
  // 在 main 退出路径上紧跟着就析构），是确定的 use-after-free。worker 的
  // WaitForSingleObject 已降到 150ms 轮询，所以 join 最坏等待约 150ms + 一次事件处理。
  for (std::thread& t : threads_) {
    if (t.joinable()) t.join();
  }
  threads_.clear();
  logInfo("health", "fs watcher stopped");
}

void HealthWatcher::handleChange(int64_t rootId, const std::string& path, const char* change) {
  // 1) 去抖：同一路径 200ms 内的同类事件只处理一次。
  // 批量写入（导入 / mock 重建 / 导出）会让 ReadDirectoryChangesW 对同一个文件
  // 连发 ADDED、REMOVED、MODIFIED，逐条处理会拖慢写入并产生状态抖动。
  const int64_t now = nowMs();
  {
    std::lock_guard<std::mutex> lock(debounceMutex_);
    auto it = lastEvent_.find(path);
    if (it != lastEvent_.end() && it->second.first == change && now - it->second.second < 200) {
      return;
    }
    if (lastEvent_.size() > 8192) lastEvent_.clear();
    lastEvent_[path] = std::make_pair(std::string(change), now);
  }

  // 2) 存在性复核：文件系统事件只是瞬时快照。原子写（temp + rename）与覆盖写
  //    都会产生短暂的 REMOVED，若磁盘上文件仍在，就不是"缺失"，必须忽略。
  //    不做这一步会出现真实的假报：写 1.7 万个文件时冒出 4000+ 条 missing 噪音。
  const bool exists = fileExists(path);
  if (std::strcmp(change, "missing") == 0 && exists) return;
  if (std::strcmp(change, "restored") == 0 && !exists) return;

  std::string err;
  PhotoRecord p;
  bool known = photos_->getByPath(path, &p, &err);
  if (!known) return;
  if (std::strcmp(change, "missing") == 0) {
    if (p.isMissing) return;  // 状态未变，不重复记 issue
    photos_->setMissing(p.id, true, &err);
    photos_->setHealth(p.id, "missing", &err);
    health_->recordIssue(p.id, "missing", path, path, &err);
  } else if (std::strcmp(change, "restored") == 0) {
    if (!p.isMissing) return;
    photos_->setMissing(p.id, false, &err);
    photos_->setHealth(p.id, "ok", &err);
    health_->closeIssuesFor(p.id, &err);
  } else {
    // modified：触发 sidecar 外部修改检测由 XMP 服务负责，这里只通知 UI
  }
  if (notify_) {
    json::JsonValue params = json::JsonValue::objectValue();
    params.setItem("rootId", rootId);
    params.setItem("path", path);
    params.setItem("change", change);
    notify_("event.fs.changed", params);
  }
}

void HealthWatcher::watchRoot(int64_t rootId, const std::string& rootPath) {
  std::wstring w = utf8ToUtf16(rootPath);
  HANDLE hDir = CreateFileW(w.c_str(), FILE_LIST_DIRECTORY,
                            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                            OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED,
                            nullptr);
  if (hDir == INVALID_HANDLE_VALUE) {
    logWarn("health", "cannot open directory for watching: " + rootPath);
    return;
  }
  std::vector<char> buffer(64 * 1024);
  OVERLAPPED ov;
  std::memset(&ov, 0, sizeof(ov));
  ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (!ov.hEvent) {
    CloseHandle(hDir);
    return;
  }
  const DWORD kFilter = FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME |
                        FILE_NOTIFY_CHANGE_SIZE | FILE_NOTIFY_CHANGE_LAST_WRITE |
                        FILE_NOTIFY_CHANGE_CREATION;
  while (running_.load()) {
    ResetEvent(ov.hEvent);
    DWORD bytesReturned = 0;
    BOOL ok = ReadDirectoryChangesW(hDir, buffer.data(), static_cast<DWORD>(buffer.size()), TRUE,
                                    kFilter, &bytesReturned, &ov, nullptr);
    if (!ok) {
      logWarn("health", "ReadDirectoryChangesW failed for " + rootPath);
      break;
    }
    // 用 150ms 轮询代替无限阻塞：stop() 现在是 join（不再 detach），
    // 轮询间隔就是退出延迟上限 —— 1s 会让每个库根线程各拖 1s 退出。
    DWORD wait = WaitForSingleObject(ov.hEvent, 150);
    if (wait != WAIT_OBJECT_0) {
      // 超时：取消本次未完成的请求，重新发起
      CancelIo(hDir);
      continue;
    }
    if (!running_.load()) break;
    DWORD transferred = 0;
    if (!GetOverlappedResult(hDir, &ov, &transferred, FALSE) || transferred == 0) continue;
    size_t offset = 0;
    while (offset + sizeof(FILE_NOTIFY_INFORMATION) <= transferred) {
      FILE_NOTIFY_INFORMATION* info =
          reinterpret_cast<FILE_NOTIFY_INFORMATION*>(buffer.data() + offset);
      std::wstring name(info->FileName, info->FileNameLength / sizeof(wchar_t));
      std::string utf8 = utf16ToUtf8(name);
      std::string full = joinPath(rootPath, utf8);
      const char* change = "modified";
      switch (info->Action) {
        case FILE_ACTION_ADDED:
        case FILE_ACTION_RENAMED_NEW_NAME:
          change = "restored";
          break;
        case FILE_ACTION_REMOVED:
        case FILE_ACTION_RENAMED_OLD_NAME:
          change = "missing";
          break;
        default:
          change = "modified";
          break;
      }
      handleChange(rootId, full, change);
      if (info->NextEntryOffset == 0) break;
      offset += info->NextEntryOffset;
    }
  }
  CancelIo(hDir);
  CloseHandle(ov.hEvent);
  CloseHandle(hDir);
}

}  // namespace health
}  // namespace lumina
