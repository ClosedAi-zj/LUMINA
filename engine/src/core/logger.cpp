// 日志实现：stderr + 按日滚动文件（7 天保留）
#include "core/logger.h"

#include <chrono>
#include <cstdio>
#include <thread>

#include "core/str_util.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace lumina {

Logger& Logger::instance() {
  static Logger logger;
  return logger;
}

void Logger::init(const std::string& logDir, LogLevel level) {
  std::lock_guard<std::mutex> lock(mutex_);
  level_ = level;
  logDir_ = logDir;
  if (!logDir_.empty()) {
    makeDirs(logDir_);
    rotateIfNeededLocked(currentLogDate());
  }
  inited_ = true;
}

void Logger::rotateIfNeededLocked(const std::string& dateTag) {
  std::string date = currentLogDate();
  if (date != currentDate_ || !file_.is_open()) {
    if (file_.is_open()) file_.close();
    currentDate_ = date;
    std::string path = joinPath(logDir_, "engine-" + date + ".log");
    file_.open(path.c_str(), std::ios::out | std::ios::app);
  }
}

void Logger::log(LogLevel level, const std::string& module, const std::string& message) {
  if (level < level_) return;
  std::lock_guard<std::mutex> lock(mutex_);
  const char* levelName = "INFO";
  switch (level) {
    case LogLevel::kDebug: levelName = "DEBUG"; break;
    case LogLevel::kInfo: levelName = "INFO"; break;
    case LogLevel::kWarn: levelName = "WARN"; break;
    case LogLevel::kError: levelName = "ERROR"; break;
  }
  std::string line = "[" + formatTimestamp(nowMs()) + "][" + levelName + "][thread-" +
                     std::to_string(std::hash<std::thread::id>{}(std::this_thread::get_id()) %
                                    100000) +
                     "][" + module + "] " + message;
  // stdout 专供协议帧，日志只落 stderr + 文件
  fprintf(stderr, "%s\n", line.c_str());
  fflush(stderr);
  if (!logDir_.empty()) {
    rotateIfNeededLocked(currentDate_);
    if (file_.is_open()) {
      file_ << line << "\n";
      file_.flush();
    }
  }
}

void Logger::debug(const std::string& module, const std::string& message) {
  log(LogLevel::kDebug, module, message);
}
void Logger::info(const std::string& module, const std::string& message) {
  log(LogLevel::kInfo, module, message);
}
void Logger::warn(const std::string& module, const std::string& message) {
  log(LogLevel::kWarn, module, message);
}
void Logger::error(const std::string& module, const std::string& message) {
  log(LogLevel::kError, module, message);
}

void Logger::shutdown() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (file_.is_open()) {
    file_.flush();
    file_.close();
  }
  inited_ = false;
}

void logDebug(const std::string& module, const std::string& message) {
  Logger::instance().debug(module, message);
}
void logInfo(const std::string& module, const std::string& message) {
  Logger::instance().info(module, message);
}
void logWarn(const std::string& module, const std::string& message) {
  Logger::instance().warn(module, message);
}
void logError(const std::string& module, const std::string& message) {
  Logger::instance().error(module, message);
}

}  // namespace lumina
