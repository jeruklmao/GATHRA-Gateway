#pragma once

#include <Arduino.h>

#include "build_config.hpp"

namespace gathra::gateway {

enum class LogLevel : uint8_t { kDebug, kInfo, kWarn, kError };

struct LogEntry {
  uint64_t uptimeMs = 0;
  LogLevel level = LogLevel::kInfo;
  char subsystem[9]{};
  char message[build::kLogMessageCapacity]{};
};

class Logger {
 public:
  bool begin();
  void log(LogLevel level, const char* subsystem, const char* format, ...)
      __attribute__((format(printf, 4, 5)));
  size_t snapshot(LogEntry* output, size_t capacity) const;
  static const char* levelName(LogLevel level);

 private:
  mutable SemaphoreHandle_t mutex_ = nullptr;
  LogEntry entries_[build::kLogCapacity]{};
  size_t size_ = 0;
  size_t next_ = 0;
};

extern Logger gLogger;

}  // namespace gathra::gateway

#define GTH_LOGD(tag, fmt, ...) \
  ::gathra::gateway::gLogger.log(::gathra::gateway::LogLevel::kDebug, tag, fmt, ##__VA_ARGS__)
#define GTH_LOGI(tag, fmt, ...) \
  ::gathra::gateway::gLogger.log(::gathra::gateway::LogLevel::kInfo, tag, fmt, ##__VA_ARGS__)
#define GTH_LOGW(tag, fmt, ...) \
  ::gathra::gateway::gLogger.log(::gathra::gateway::LogLevel::kWarn, tag, fmt, ##__VA_ARGS__)
#define GTH_LOGE(tag, fmt, ...) \
  ::gathra::gateway::gLogger.log(::gathra::gateway::LogLevel::kError, tag, fmt, ##__VA_ARGS__)
