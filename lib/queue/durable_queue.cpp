#ifdef ARDUINO

#include "durable_queue.hpp"

#include <Arduino.h>
#include <LittleFS.h>
#include <errno.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "build_config.hpp"
#include "protocol.hpp"

namespace gathra::gateway {
namespace {

constexpr char kQueueDirectory[] = "/queue";
constexpr char kRecentPath[] = "/queue/recent.bin";
constexpr char kRecentTempPath[] = "/queue/recent.tmp";
constexpr char kFilesystemBasePath[] = "/littlefs";
constexpr char kFilesystemPartitionLabel[] = "littlefs";
constexpr uint32_t kRecentMagic = 0x47544431U;  // GTD1

class Lock {
 public:
  explicit Lock(SemaphoreHandle_t mutex) : mutex_(mutex) {
    locked_ = mutex_ != nullptr && xSemaphoreTake(mutex_, portMAX_DELAY) == pdTRUE;
  }
  ~Lock() {
    if (locked_) xSemaphoreGive(mutex_);
  }
  explicit operator bool() const { return locked_; }

 private:
  SemaphoreHandle_t mutex_;
  bool locked_ = false;
};

void writeU32(std::vector<uint8_t>& bytes, uint32_t value) {
  bytes.push_back(static_cast<uint8_t>(value >> 24U));
  bytes.push_back(static_cast<uint8_t>(value >> 16U));
  bytes.push_back(static_cast<uint8_t>(value >> 8U));
  bytes.push_back(static_cast<uint8_t>(value));
}

bool readU32(const uint8_t*& cursor, const uint8_t* end, uint32_t& value) {
  if (end - cursor < 4) return false;
  value = (static_cast<uint32_t>(cursor[0]) << 24U) |
          (static_cast<uint32_t>(cursor[1]) << 16U) |
          (static_cast<uint32_t>(cursor[2]) << 8U) | cursor[3];
  cursor += 4;
  return true;
}

String mountedPath(const char* logicalPath) {
  return String(kFilesystemBasePath) + logicalPath;
}

bool silentlyUnlink(const char* logicalPath) {
  const String path = mountedPath(logicalPath);
  errno = 0;
  return ::unlink(path.c_str()) == 0 || errno == ENOENT;
}

// LittleFS.exists() opens the target and emits an ESP-IDF error for a normal
// ENOENT result. Queue recovery treats an absent optional file as expected, so
// use the mounted VFS directly to keep diagnostics reserved for real failures.
int filePresence(const char* logicalPath) {
  struct stat info {};
  const String path = mountedPath(logicalPath);
  errno = 0;
  if (::stat(path.c_str(), &info) == 0) return 1;
  return errno == ENOENT ? 0 : -1;
}

}  // namespace

bool LittleFsQueueStorage::begin(bool formatOnFailure) {
  if (!LittleFS.begin(formatOnFailure, kFilesystemBasePath, 10,
                      kFilesystemPartitionLabel)) {
    return false;
  }
  const int directoryPresence = filePresence(kQueueDirectory);
  if (directoryPresence < 0 ||
      (directoryPresence == 0 && !LittleFS.mkdir(kQueueDirectory))) {
    return false;
  }
  return true;
}

bool LittleFsQueueStorage::listRecordIds(std::vector<uint64_t>& recordIds,
                                         uint32_t& corruptNames) {
  recordIds.clear();
  corruptNames = 0;
  File directory = LittleFS.open(kQueueDirectory);
  if (!directory || !directory.isDirectory()) return false;
  std::vector<String> invalidPaths;
  File file = directory.openNextFile();
  while (file) {
    if (!file.isDirectory()) {
      String path(file.name());
      if (path.endsWith(".rec")) {
        const int slash = path.lastIndexOf('/');
        const String name = path.substring(slash + 1);
        uint64_t recordId = 0;
        bool valid = name.length() == 20U && name.endsWith(".rec");
        for (size_t index = 0; valid && index < 16U; ++index) {
          const char value = name[index];
          uint8_t nibble = 0;
          if (value >= '0' && value <= '9') nibble = value - '0';
          else if (value >= 'a' && value <= 'f') nibble = value - 'a' + 10U;
          else valid = false;
          recordId = (recordId << 4U) | nibble;
        }
        if (valid && recordId != 0U) recordIds.push_back(recordId);
        else invalidPaths.push_back(path);
      }
    }
    file = directory.openNextFile();
  }
  for (const String& path : invalidPaths) {
    ++corruptNames;
    if (!LittleFS.remove(path)) return false;
  }
  return true;
}

bool LittleFsQueueStorage::readRecord(const std::string& name,
                                      std::vector<uint8_t>& bytes) {
  File file = LittleFS.open(fullPath(name), FILE_READ);
  if (!file || file.isDirectory() || file.size() > QueueRecordCodec::kMaximumEncodedBytes) {
    return false;
  }
  bytes.resize(file.size());
  return file.read(bytes.data(), bytes.size()) == bytes.size();
}

bool LittleFsQueueStorage::writeRecordAtomic(
    uint64_t recordId, const std::vector<uint8_t>& bytes) {
  char baseName[32]{};
  snprintf(baseName, sizeof(baseName), "%016llx",
           static_cast<unsigned long long>(recordId));
  const String temporary = String(kQueueDirectory) + "/" + baseName + ".tmp";
  const String finalPath = String(kQueueDirectory) + "/" + baseName + ".rec";
  if (!silentlyUnlink(temporary.c_str())) return false;
  File file = LittleFS.open(temporary, FILE_WRITE);
  if (!file) return false;
  const bool complete = file.write(bytes.data(), bytes.size()) == bytes.size();
  file.flush();
  file.close();
  if (!complete) {
    (void)silentlyUnlink(temporary.c_str());
    return false;
  }
  if (!LittleFS.rename(temporary, finalPath)) {
    (void)silentlyUnlink(temporary.c_str());
    return false;
  }
  return true;
}

bool LittleFsQueueStorage::removeRecord(const std::string& name) {
  return LittleFS.remove(fullPath(name));
}

void LittleFsQueueStorage::cleanupTemporaryFiles() {
  File directory = LittleFS.open(kQueueDirectory);
  if (!directory || !directory.isDirectory()) return;
  std::vector<String> temporary;
  File file = directory.openNextFile();
  while (file) {
    String path(file.name());
    if (!file.isDirectory() && path.endsWith(".tmp")) temporary.push_back(path);
    file = directory.openNextFile();
  }
  for (const String& path : temporary) (void)LittleFS.remove(path);
}

uint64_t LittleFsQueueStorage::totalBytes() const { return LittleFS.totalBytes(); }
uint64_t LittleFsQueueStorage::usedBytes() const { return LittleFS.usedBytes(); }

String LittleFsQueueStorage::fullPath(const std::string& name) {
  return String(kQueueDirectory) + "/" + name.c_str();
}

bool DurableQueue::begin() {
  mutex_ = xSemaphoreCreateMutex();
  if (mutex_ == nullptr || !preferences_.begin("gw-queue", false)) {
    return false;
  }
  const bool initializedBefore = preferences_.getBool("fs-ready", false);
  if (!storage_.begin(!initializedBefore)) return false;
  if (!initializedBefore && preferences_.putBool("fs-ready", true) != sizeof(uint8_t)) {
    return false;
  }
  core_ = std::make_unique<DurableQueueCore>(storage_, deriveCapacity(storage_.totalBytes()));
  if (!core_->recover()) return false;
  observedCoreDropped_ = core_->stats().droppedOldest;
  observedCoreErrors_ = core_->stats().storageErrors + core_->stats().corruptRecords;
  (void)loadCounters();
  stats_.recordsDroppedOldest += observedCoreDropped_;
  stats_.filesystemErrors += observedCoreErrors_;
  const bool recentReady = loadRecent();
  if (!recentReady) {
    recent_.clear();
    ++stats_.filesystemErrors;
    if (!silentlyUnlink(kRecentPath)) {
      ++stats_.filesystemErrors;
    }
  }
  if (observedCoreDropped_ > 0U || observedCoreErrors_ > 0U || !recentReady) {
    (void)persistCounters();
  }
  healthy_ = true;
  return true;
}

PacketQueueSink::EnqueueOutcome DurableQueue::enqueue(QueueRecord& record) {
  Lock lock(mutex_);
  if (!lock || !healthy_ || core_ == nullptr) return {};
  const uint32_t beforeDropped = core_->stats().droppedOldest;
  const uint32_t beforeErrors = core_->stats().storageErrors;
  const bool result = core_->enqueue(record);
  stats_.recordsDroppedOldest += core_->stats().droppedOldest - beforeDropped;
  stats_.filesystemErrors += core_->stats().storageErrors - beforeErrors;
  if (core_->stats().droppedOldest != beforeDropped) {
    (void)persistCounters();
  }
  return {result, core_->stats().droppedOldest != beforeDropped};
}

bool DurableQueue::peek(size_t limit, std::vector<QueueRecord>& records) {
  Lock lock(mutex_);
  return lock && healthy_ && core_ != nullptr && core_->peek(limit, records);
}

bool DurableQueue::contains(const TelemetryKey& key) {
  Lock lock(mutex_);
  if (!lock || !healthy_ || core_ == nullptr) return false;
  const uint32_t beforeErrors = core_->stats().storageErrors;
  const bool contained = recent_.contains(key) || core_->contains(key);
  stats_.filesystemErrors += core_->stats().storageErrors - beforeErrors;
  return contained;
}

bool DurableQueue::finish(const std::vector<QueueTerminalAction>& actions) {
  Lock lock(mutex_);
  if (!lock || !healthy_ || core_ == nullptr) return false;
  bool success = true;
  bool changedRecent = false;
  for (const QueueTerminalAction& action : actions) {
    if (!core_->remove(action.recordId)) {
      ++stats_.filesystemErrors;
      success = false;
      continue;
    }
    changedRecent = recent_.remember(action.key) || changedRecent;
    if (action.permanentlyRejected) {
      ++stats_.recordsPermanentlyRejected;
    } else {
      ++stats_.recordsUploaded;
    }
    ++mutationsSinceCheckpoint_;
  }
  if (changedRecent && !persistRecent()) {
    ++stats_.filesystemErrors;
    success = false;
  }
  checkpointCountersUnlocked(false);
  return success;
}

void DurableQueue::recordDuplicate() {
  Lock lock(mutex_);
  if (!lock) return;
  ++stats_.recordsDeduplicated;
  ++mutationsSinceCheckpoint_;
}

void DurableQueue::recordUploadRetry() {
  Lock lock(mutex_);
  if (!lock) return;
  ++stats_.uploadRetries;
  ++mutationsSinceCheckpoint_;
  checkpointCountersUnlocked(false);
}

void DurableQueue::recordFilesystemError() {
  Lock lock(mutex_);
  if (!lock) return;
  ++stats_.filesystemErrors;
  ++mutationsSinceCheckpoint_;
}

void DurableQueue::checkpointCounters(bool force) {
  Lock lock(mutex_);
  if (lock) checkpointCountersUnlocked(force);
}

void DurableQueue::checkpointCountersUnlocked(bool force) {
  const uint32_t now = millis();
  if (!force && mutationsSinceCheckpoint_ < 16U && now - lastCheckpointMs_ < 60'000U) {
    return;
  }
  (void)persistCounters();
}

size_t DurableQueue::size() const {
  Lock lock(mutex_);
  return lock && core_ != nullptr ? core_->size() : 0U;
}

size_t DurableQueue::capacity() const {
  Lock lock(mutex_);
  return lock && core_ != nullptr ? core_->capacity() : 0U;
}

uint64_t DurableQueue::filesystemTotalBytes() const {
  Lock lock(mutex_);
  return lock ? storage_.totalBytes() : 0U;
}

uint64_t DurableQueue::filesystemUsedBytes() const {
  Lock lock(mutex_);
  return lock ? storage_.usedBytes() : 0U;
}

QueueOperationalStats DurableQueue::operationalStats() const {
  Lock lock(mutex_);
  return lock ? stats_ : QueueOperationalStats{};
}

bool DurableQueue::loadRecent() {
  const int presence = filePresence(kRecentPath);
  if (presence == 0) return true;
  if (presence < 0) return false;
  File file = LittleFS.open(kRecentPath, FILE_READ);
  if (!file || file.size() < 9U || file.size() > 4096U) return false;
  std::vector<uint8_t> bytes(file.size());
  if (file.read(bytes.data(), bytes.size()) != bytes.size()) return false;
  const uint32_t storedCrc = (static_cast<uint32_t>(bytes[bytes.size() - 4U]) << 24U) |
                             (static_cast<uint32_t>(bytes[bytes.size() - 3U]) << 16U) |
                             (static_cast<uint32_t>(bytes[bytes.size() - 2U]) << 8U) |
                             bytes[bytes.size() - 1U];
  if (storedCrc != QueueRecordCodec::crc32(bytes.data(), bytes.size() - 4U)) return false;
  const uint8_t* cursor = bytes.data();
  const uint8_t* end = bytes.data() + bytes.size() - 4U;
  uint32_t magic = 0;
  if (!readU32(cursor, end, magic) || magic != kRecentMagic || cursor >= end) return false;
  const uint8_t count = *cursor++;
  if (count > build::kRecentDedupCapacity) return false;
  recent_.clear();
  for (uint8_t index = 0; index < count; ++index) {
    if (cursor >= end) return false;
    const uint8_t length = *cursor++;
    if (length == 0U || length >= build::kNodeIdCapacity || end - cursor < length + 8) return false;
    char nodeId[build::kNodeIdCapacity]{};
    memcpy(nodeId, cursor, length);
    cursor += length;
    uint32_t boot = 0, sequence = 0;
    if (!protocol::nodeIdValid(nodeId) || !readU32(cursor, end, boot) ||
        !readU32(cursor, end, sequence)) return false;
    recent_.remember(makeTelemetryKey(nodeId, boot, sequence));
  }
  return cursor == end;
}

bool DurableQueue::persistRecent() {
  std::vector<uint8_t> bytes;
  bytes.reserve(8U + recent_.size() * 34U);
  writeU32(bytes, kRecentMagic);
  bytes.push_back(static_cast<uint8_t>(recent_.size()));
  for (size_t index = 0; index < recent_.size(); ++index) {
    const TelemetryKey& key = recent_.oldestAt(index);
    const size_t length = strnlen(key.nodeId, sizeof(key.nodeId));
    bytes.push_back(static_cast<uint8_t>(length));
    bytes.insert(bytes.end(), key.nodeId, key.nodeId + length);
    writeU32(bytes, key.persistentSessionId);
    writeU32(bytes, key.sequence);
  }
  writeU32(bytes, QueueRecordCodec::crc32(bytes.data(), bytes.size()));
  if (!silentlyUnlink(kRecentTempPath)) return false;
  File file = LittleFS.open(kRecentTempPath, FILE_WRITE);
  if (!file) return false;
  const bool complete = file.write(bytes.data(), bytes.size()) == bytes.size();
  file.flush();
  file.close();
  if (!complete) {
    (void)silentlyUnlink(kRecentTempPath);
    return false;
  }
  return LittleFS.rename(kRecentTempPath, kRecentPath);
}

bool DurableQueue::loadCounters() {
  stats_.recordsUploaded = preferences_.getULong64("uploaded", 0);
  stats_.uploadRetries = preferences_.getULong64("retries", 0);
  stats_.recordsDeduplicated = preferences_.getULong64("dedup", 0);
  stats_.recordsPermanentlyRejected = preferences_.getULong64("rejected", 0);
  stats_.recordsDroppedOldest = preferences_.getULong64("dropped", 0);
  stats_.filesystemErrors = preferences_.getULong64("fs-errors", 0);
  return true;
}

bool DurableQueue::persistCounters() {
  bool okay = true;
  okay = preferences_.putULong64("uploaded", stats_.recordsUploaded) > 0 && okay;
  okay = preferences_.putULong64("retries", stats_.uploadRetries) > 0 && okay;
  okay = preferences_.putULong64("dedup", stats_.recordsDeduplicated) > 0 && okay;
  okay = preferences_.putULong64("rejected", stats_.recordsPermanentlyRejected) > 0 && okay;
  okay = preferences_.putULong64("dropped", stats_.recordsDroppedOldest) > 0 && okay;
  okay = preferences_.putULong64("fs-errors", stats_.filesystemErrors) > 0 && okay;
  if (okay) {
    mutationsSinceCheckpoint_ = 0;
    lastCheckpointMs_ = millis();
  }
  return okay;
}

uint32_t DurableQueue::deriveCapacity(uint64_t filesystemBytes) {
  if (filesystemBytes <= build::kQueueReservedBytes) return 1U;
  const uint64_t perRecord = QueueRecordCodec::kMaximumEncodedBytes +
                             build::kEstimatedLittleFsFileOverhead;
  uint64_t capacity = (filesystemBytes - build::kQueueReservedBytes) / perRecord;
  // Keep an additional 20% free for copy-on-write behavior and atomic temp files.
  capacity = capacity * 4U / 5U;
  if (capacity == 0U) capacity = 1U;
  if (capacity > build::kQueueCapacityHardMax) capacity = build::kQueueCapacityHardMax;
  return static_cast<uint32_t>(capacity);
}

}  // namespace gathra::gateway

#endif  // ARDUINO
