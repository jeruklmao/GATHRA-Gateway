#pragma once

#include <stddef.h>
#include <stdint.h>

#include "build_config.hpp"

namespace gathra::gateway {

struct TelemetryKey {
  char nodeId[build::kNodeIdCapacity]{};
  uint32_t bootSessionId = 0;
  uint32_t sequence = 0;
};

TelemetryKey makeTelemetryKey(const char* nodeId, uint32_t bootSessionId,
                              uint32_t sequence);
bool telemetryKeyEqual(const TelemetryKey& lhs, const TelemetryKey& rhs);

class RecentDeduplicator {
 public:
  explicit RecentDeduplicator(size_t capacity = build::kRecentDedupCapacity);
  bool contains(const TelemetryKey& key) const;
  bool remember(const TelemetryKey& key);
  void clear();
  size_t size() const { return size_; }
  size_t capacity() const { return capacity_; }
  const TelemetryKey& oldestAt(size_t index) const;

 private:
  TelemetryKey keys_[build::kRecentDedupCapacity]{};
  size_t capacity_ = build::kRecentDedupCapacity;
  size_t size_ = 0;
  size_t next_ = 0;
};

}  // namespace gathra::gateway
