#include "durable_queue_core.hpp"

#include <algorithm>
#include <stdio.h>
#include <string.h>

#include "protocol.hpp"

namespace gathra::gateway {

DurableQueueCore::DurableQueueCore(QueueStorage& storage, size_t capacity)
    : storage_(storage), capacity_(capacity == 0U ? 1U : capacity) {}

bool DurableQueueCore::recover() {
  entries_.clear();
  nextRecordId_ = 1;
  storage_.cleanupTemporaryFiles();
  std::vector<uint64_t> recordIds;
  uint32_t corruptNames = 0;
  if (!storage_.listRecordIds(recordIds, corruptNames)) {
    ++stats_.storageErrors;
    return false;
  }
  stats_.corruptRecords += corruptNames;
  entries_.reserve(std::min(recordIds.size(), capacity_ + 1U));
  for (const uint64_t listedRecordId : recordIds) {
    const std::string name = recordName(listedRecordId);
    std::vector<uint8_t> bytes;
    QueueRecord record{};
    protocol::TelemetryPacket telemetry{};
    if (!storage_.readRecord(name, bytes) ||
        !QueueRecordCodec::decode(bytes.data(), bytes.size(), record) ||
        record.recordId != listedRecordId ||
        protocol::decodeTelemetry(record.rawPayload, record.payloadLength, telemetry) !=
            protocol::DecodeStatus::kOk) {
      ++stats_.corruptRecords;
      if (!storage_.removeRecord(name)) ++stats_.storageErrors;
      continue;
    }
    const TelemetryKey key = makeTelemetryKey(
        telemetry.nodeId, telemetry.persistentSessionId, telemetry.sequence);
    entries_.push_back({record.recordId, keyHash(key)});
    if (record.recordId >= nextRecordId_) nextRecordId_ = record.recordId + 1U;
  }
  std::sort(entries_.begin(), entries_.end(),
            [](const Entry& lhs, const Entry& rhs) {
              return lhs.recordId < rhs.recordId;
            });
  while (entries_.size() > capacity_) {
    if (!storage_.removeRecord(recordName(entries_.front().recordId))) {
      ++stats_.storageErrors;
      return false;
    }
    entries_.erase(entries_.begin());
    ++stats_.droppedOldest;
  }
  stats_.recoveredRecords = static_cast<uint32_t>(entries_.size());
  return true;
}

bool DurableQueueCore::enqueue(QueueRecord& record) {
  if (nextRecordId_ == 0U) {
    ++stats_.storageErrors;
    return false;
  }
  protocol::TelemetryPacket telemetry{};
  if (protocol::decodeTelemetry(record.rawPayload, record.payloadLength, telemetry) !=
      protocol::DecodeStatus::kOk) {
    return false;
  }
  record.recordId = nextRecordId_;
  std::vector<uint8_t> bytes;
  if (!QueueRecordCodec::encode(record, bytes)) return false;

  bool wrote = storage_.writeRecordAtomic(record.recordId, bytes);
  if (!wrote && entries_.size() >= capacity_) {
    // Last-resort full-filesystem path. The drop is explicit and the caller
    // receives false (and therefore sends no ACK) if the retry still fails.
    if (!storage_.removeRecord(recordName(entries_.front().recordId))) {
      ++stats_.storageErrors;
      return false;
    }
    entries_.erase(entries_.begin());
    ++stats_.droppedOldest;
    wrote = storage_.writeRecordAtomic(record.recordId, bytes);
  }
  if (!wrote) {
    ++stats_.storageErrors;
    return false;
  }

  if (entries_.size() >= capacity_) {
    if (!storage_.removeRecord(recordName(entries_.front().recordId))) {
      // The newest record is durable, but boundedness could not be restored.
      // Remove it and withhold ACK so no acknowledged record is ambiguous.
      (void)storage_.removeRecord(recordName(record.recordId));
      ++stats_.storageErrors;
      return false;
    }
    entries_.erase(entries_.begin());
    ++stats_.droppedOldest;
  }
  const TelemetryKey key = makeTelemetryKey(
      telemetry.nodeId, telemetry.persistentSessionId, telemetry.sequence);
  entries_.push_back({record.recordId, keyHash(key)});
  ++nextRecordId_;
  return true;
}

bool DurableQueueCore::peek(size_t limit, std::vector<QueueRecord>& records) {
  records.clear();
  const size_t count = std::min(limit, entries_.size());
  records.reserve(count);
  for (size_t index = 0; index < count; ++index) {
    std::vector<uint8_t> bytes;
    QueueRecord record{};
    if (!storage_.readRecord(recordName(entries_[index].recordId), bytes) ||
        !QueueRecordCodec::decode(bytes.data(), bytes.size(), record)) {
      ++stats_.storageErrors;
      records.clear();
      return false;
    }
    records.push_back(record);
  }
  return true;
}

bool DurableQueueCore::remove(uint64_t recordId) {
  const auto found = std::find_if(entries_.begin(), entries_.end(),
                                  [recordId](const Entry& entry) {
                                    return entry.recordId == recordId;
                                  });
  if (found == entries_.end()) return false;
  if (!storage_.removeRecord(recordName(found->recordId))) {
    ++stats_.storageErrors;
    return false;
  }
  entries_.erase(found);
  return true;
}

bool DurableQueueCore::contains(const TelemetryKey& key) {
  const uint64_t expectedHash = keyHash(key);
  for (const Entry& entry : entries_) {
    if (entry.keyHash != expectedHash) continue;
    std::vector<uint8_t> bytes;
    QueueRecord record{};
    protocol::TelemetryPacket telemetry{};
    if (!storage_.readRecord(recordName(entry.recordId), bytes) ||
        !QueueRecordCodec::decode(bytes.data(), bytes.size(), record) ||
        protocol::decodeTelemetry(record.rawPayload, record.payloadLength,
                                  telemetry) != protocol::DecodeStatus::kOk) {
      ++stats_.storageErrors;
      continue;
    }
    const TelemetryKey stored = makeTelemetryKey(
        telemetry.nodeId, telemetry.persistentSessionId, telemetry.sequence);
    if (telemetryKeyEqual(stored, key)) return true;
  }
  return false;
}

std::string DurableQueueCore::recordName(uint64_t recordId) {
  char name[32]{};
  snprintf(name, sizeof(name), "%016llx.rec",
           static_cast<unsigned long long>(recordId));
  return name;
}

uint64_t DurableQueueCore::keyHash(const TelemetryKey& key) {
  constexpr uint64_t kOffset = 14695981039346656037ULL;
  constexpr uint64_t kPrime = 1099511628211ULL;
  uint64_t hash = kOffset;
  const size_t length = strnlen(key.nodeId, sizeof(key.nodeId));
  const auto mix = [&hash](uint8_t value) {
    hash ^= value;
    hash *= kPrime;
  };
  mix(static_cast<uint8_t>(length));
  for (size_t index = 0; index < length; ++index) {
    mix(static_cast<uint8_t>(key.nodeId[index]));
  }
  for (int shift = 24; shift >= 0; shift -= 8) {
    mix(static_cast<uint8_t>(key.persistentSessionId >> shift));
  }
  for (int shift = 24; shift >= 0; shift -= 8) {
    mix(static_cast<uint8_t>(key.sequence >> shift));
  }
  return hash;
}

}  // namespace gathra::gateway
