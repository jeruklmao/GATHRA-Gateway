#pragma once

#include <stddef.h>
#include <stdint.h>

#include <string>
#include <vector>

#include "deduplicator.hpp"
#include "queue_record.hpp"

namespace gathra::gateway {

class QueueStorage {
 public:
  virtual ~QueueStorage() = default;
  virtual bool listRecordIds(std::vector<uint64_t>& recordIds,
                             uint32_t& corruptNames) = 0;
  virtual bool readRecord(const std::string& name, std::vector<uint8_t>& bytes) = 0;
  virtual bool writeRecordAtomic(uint64_t recordId,
                                 const std::vector<uint8_t>& bytes) = 0;
  virtual bool removeRecord(const std::string& name) = 0;
  virtual void cleanupTemporaryFiles() = 0;
};

struct QueueCoreStats {
  uint32_t recoveredRecords = 0;
  uint32_t corruptRecords = 0;
  uint32_t droppedOldest = 0;
  uint32_t storageErrors = 0;
};

class DurableQueueCore {
 public:
  DurableQueueCore(QueueStorage& storage, size_t capacity);
  bool recover();
  bool enqueue(QueueRecord& record);
  bool peek(size_t limit, std::vector<QueueRecord>& records);
  bool remove(uint64_t recordId);
  bool contains(const TelemetryKey& key);
  size_t size() const { return entries_.size(); }
  size_t capacity() const { return capacity_; }
  const QueueCoreStats& stats() const { return stats_; }

 private:
  struct Entry {
    uint64_t recordId = 0;
    uint64_t keyHash = 0;
  };

  static std::string recordName(uint64_t recordId);
  static uint64_t keyHash(const TelemetryKey& key);
  QueueStorage& storage_;
  size_t capacity_;
  uint64_t nextRecordId_ = 1;
  std::vector<Entry> entries_;
  QueueCoreStats stats_{};
};

}  // namespace gathra::gateway
