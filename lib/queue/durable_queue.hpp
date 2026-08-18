#pragma once

#include <FS.h>
#include <Preferences.h>

#include <memory>
#include <vector>

#include "deduplicator.hpp"
#include "durable_queue_core.hpp"
#include "packet_processor.hpp"

namespace gathra::gateway {

struct QueueOperationalStats {
  uint64_t recordsUploaded = 0;
  uint64_t uploadRetries = 0;
  uint64_t recordsDeduplicated = 0;
  uint64_t recordsPermanentlyRejected = 0;
  uint64_t recordsDroppedOldest = 0;
  uint64_t filesystemErrors = 0;
};

struct QueueTerminalAction {
  uint64_t recordId = 0;
  TelemetryKey key{};
  bool permanentlyRejected = false;
};

class LittleFsQueueStorage final : public QueueStorage {
 public:
  bool begin(bool formatOnFailure);
  bool listRecordIds(std::vector<uint64_t>& recordIds,
                     uint32_t& corruptNames) override;
  bool readRecord(const std::string& name, std::vector<uint8_t>& bytes) override;
  bool writeRecordAtomic(uint64_t recordId,
                         const std::vector<uint8_t>& bytes) override;
  bool removeRecord(const std::string& name) override;
  void cleanupTemporaryFiles() override;
  uint64_t totalBytes() const;
  uint64_t usedBytes() const;

 private:
  static String fullPath(const std::string& name);
};

class DurableQueue final : public PacketQueueSink {
 public:
  bool begin();
  EnqueueOutcome enqueue(QueueRecord& record);
  bool peek(size_t limit, std::vector<QueueRecord>& records);
  bool contains(const TelemetryKey& key);
  bool finish(const std::vector<QueueTerminalAction>& actions);
  void recordDuplicate();
  void recordUploadRetry();
  void recordFilesystemError();
  void checkpointCounters(bool force = false);

  size_t size() const;
  size_t capacity() const;
  uint64_t filesystemTotalBytes() const;
  uint64_t filesystemUsedBytes() const;
  QueueOperationalStats operationalStats() const;
  bool healthy() const { return healthy_; }

  bool containsKey(const TelemetryKey& key) override { return contains(key); }
  EnqueueOutcome durablyEnqueue(QueueRecord& record) override {
    return enqueue(record);
  }
  void noteDuplicate() override { recordDuplicate(); }

 private:
  bool loadRecent();
  bool persistRecent();
  bool loadCounters();
  bool persistCounters();
  void checkpointCountersUnlocked(bool force);
  static uint32_t deriveCapacity(uint64_t filesystemBytes);

  LittleFsQueueStorage storage_;
  std::unique_ptr<DurableQueueCore> core_;
  RecentDeduplicator recent_{};
  Preferences preferences_;
  SemaphoreHandle_t mutex_ = nullptr;
  QueueOperationalStats stats_{};
  uint32_t mutationsSinceCheckpoint_ = 0;
  uint32_t lastCheckpointMs_ = 0;
  uint32_t observedCoreDropped_ = 0;
  uint32_t observedCoreErrors_ = 0;
  bool healthy_ = false;
};

}  // namespace gathra::gateway
