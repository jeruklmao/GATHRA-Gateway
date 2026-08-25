#include "deduplicator.hpp"

#include <string.h>

namespace gathra::gateway {

TelemetryKey makeTelemetryKey(const char* nodeId, uint32_t persistentSessionId,
                              uint32_t sequence) {
  TelemetryKey key{};
  if (nodeId != nullptr) strncpy(key.nodeId, nodeId, sizeof(key.nodeId) - 1U);
  key.persistentSessionId = persistentSessionId;
  key.sequence = sequence;
  return key;
}

bool telemetryKeyEqual(const TelemetryKey& lhs, const TelemetryKey& rhs) {
  return strncmp(lhs.nodeId, rhs.nodeId, build::kNodeIdCapacity) == 0 &&
         lhs.persistentSessionId == rhs.persistentSessionId &&
         lhs.sequence == rhs.sequence;
}

RecentDeduplicator::RecentDeduplicator(size_t capacity) {
  capacity_ = capacity == 0U ? 1U : capacity;
  if (capacity_ > build::kRecentDedupCapacity) capacity_ = build::kRecentDedupCapacity;
}

bool RecentDeduplicator::contains(const TelemetryKey& key) const {
  for (size_t index = 0; index < size_; ++index) {
    if (telemetryKeyEqual(oldestAt(index), key)) return true;
  }
  return false;
}

bool RecentDeduplicator::remember(const TelemetryKey& key) {
  if (contains(key)) return false;
  keys_[next_] = key;
  next_ = (next_ + 1U) % capacity_;
  if (size_ < capacity_) ++size_;
  return true;
}

void RecentDeduplicator::clear() {
  size_ = 0;
  next_ = 0;
}

const TelemetryKey& RecentDeduplicator::oldestAt(size_t index) const {
  const size_t oldest = size_ < capacity_ ? 0U : next_;
  return keys_[(oldest + index) % capacity_];
}

}  // namespace gathra::gateway
