// 日志：[时间][级别][线程][模块] msg；stderr + {home}/logs/engine-YYYYMMDD.log（stdout 永不混入）
#ifndef LUMINA_CORE_LOGGER_H_
#define LUMINA_CORE_LOGGER_H_

#include <fstream>
#include <mutex>
#include <string>

namespace lumina {

enum class LogLevel { kDebug = 0, kInfo = 1, kWarn = 2, kError = 3 };

class Logger {
 public:
  static Logger& instance();

  // 初始化日志目录（{home}/logs）与级别；线程安全，可重复调用
  void init(const std::string& logDir, LogLevel level = LogLevel::kInfo);
  void setLevel(LogLevel level) { level_ = level; }
  LogLevel level() const { return level_; }
  std::string logDir() const { return logDir_; }

  void log(LogLevel level, const std::string& module, const std::string& message);
  void debug(const std::string& module, const std::string& message);
  void info(const std::string& module, const std::string& message);
  void warn(const std::string& module, const std::string& message);
  void error(const std::string& module, const std::string& message);

  void shutdown();

 private:
  Logger() : level_(LogLevel::kInfo), inited_(false) {}
  Logger(const Logger&) = delete;
  Logger& operator=(const Logger&) = delete;

  void rotateIfNeededLocked(const std::string& dateTag);

  std::mutex mutex_;
  std::string logDir_;
  std::string currentDate_;
  std::ofstream file_;
  LogLevel level_;
  bool inited_;
};

// 便捷函数（模块名固定字面量时使用）
void logDebug(const std::string& module, const std::string& message);
void logInfo(const std::string& module, const std::string& message);
void logWarn(const std::string& module, const std::string& message);
void logError(const std::string& module, const std::string& message);

}  // namespace lumina

#endif  // LUMINA_CORE_LOGGER_H_
