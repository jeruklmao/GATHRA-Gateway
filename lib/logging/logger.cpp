#ifdef ARDUINO

#include "logger.hpp"

#include <stdarg.h>
#include <string.h>

#include "esp_timer.h"

namespace gathra::gateway {

Logger gLogger;

bool Logger::begin() {
  mutex_ = xSemaphoreCreateMutex();
  return mutex_ != nullptr;
}

void Logger::log(LogLevel level, const char* subsystem, const char* format, ...) {
  LogEntry entry{};
  entry.uptimeMs = static_cast<uint64_t>(esp_timer_get_time()) / 1000ULL;
  entry.level = level;
  strncpy(entry.subsystem, subsystem == nullptr ? "APP" : subsystem,
          sizeof(entry.subsystem) - 1U);
  va_list arguments;
  va_start(arguments, format);
  vsnprintf(entry.message, sizeof(entry.message), format, arguments);
  va_end(arguments);

  Serial.printf("[%s][%s] %s\n", levelName(level), entry.subsystem, entry.message);
  if (mutex_ == nullptr || xSemaphoreTake(mutex_, pdMS_TO_TICKS(10)) != pdTRUE) return;
  entries_[next_] = entry;
  next_ = (next_ + 1U) % build::kLogCapacity;
  if (size_ < build::kLogCapacity) ++size_;
  xSemaphoreGive(mutex_);
}

size_t Logger::snapshot(LogEntry* output, size_t capacity) const {
  if (output == nullptr || capacity == 0U || mutex_ == nullptr ||
      xSemaphoreTake(mutex_, pdMS_TO_TICKS(50)) != pdTRUE) {
    return 0;
  }
  const size_t count = size_ < capacity ? size_ : capacity;
  const size_t oldest = size_ < build::kLogCapacity ? 0U : next_;
  const size_t skip = size_ - count;
  for (size_t index = 0; index < count; ++index) {
    output[index] = entries_[(oldest + skip + index) % build::kLogCapacity];
  }
  xSemaphoreGive(mutex_);
  return count;
}

const char* Logger::levelName(LogLevel level) {
  switch (level) {
    case LogLevel::kDebug: return "DEBUG";
    case LogLevel::kInfo: return "INFO";
    case LogLevel::kWarn: return "WARN";
    case LogLevel::kError: return "ERROR";
  }
  return "UNKNOWN";
}

}  // namespace gathra::gateway

#endif  // ARDUINO
