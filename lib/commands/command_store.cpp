#include "command_store.hpp"

#include <stddef.h>
#include <string.h>

namespace gathra::gateway {

namespace {
#ifdef ARDUINO
class StoreLock {
 public:
  explicit StoreLock(SemaphoreHandle_t mutex) : mutex_(mutex) {
    locked_ = mutex_ != nullptr &&
              xSemaphoreTake(mutex_, portMAX_DELAY) == pdTRUE;
  }
  ~StoreLock() {
    if (locked_) xSemaphoreGive(mutex_);
  }
  explicit operator bool() const { return locked_; }

 private:
  SemaphoreHandle_t mutex_ = nullptr;
  bool locked_ = false;
};
#endif
}  // namespace

uint32_t CommandStore::checksum(const CommandStoreRecord& record) {
  uint32_t hash = 2166136261U;
  const auto* bytes = reinterpret_cast<const uint8_t*>(&record);
  for (size_t i = 0; i < offsetof(CommandStoreRecord, checksum); ++i) {
    hash ^= bytes[i];
    hash *= 16777619U;
  }
  return hash;
}

void CommandStore::initialize(CommandStoreRecord& record) {
  memset(&record, 0, sizeof(record));
  record.magic = kCommandStoreMagic;
  record.schemaVersion = kCommandStoreSchema;
  record.structureSize = sizeof(record);
  record.nextCommandId = 1U;
  record.command.createdUnixMs = -1;
  record.command.lastSentUnixMs = -1;
  record.command.result = protocol::CommandResultCode::kNone;
  record.checksum = checksum(record);
}

bool CommandStore::valid(const CommandStoreRecord& record) {
  return record.magic == kCommandStoreMagic &&
         record.schemaVersion == kCommandStoreSchema &&
         record.structureSize == sizeof(record) && record.nextCommandId != 0U &&
         static_cast<uint8_t>(record.command.type) <=
             static_cast<uint8_t>(protocol::CommandType::kSetPollIntervalMinutes) &&
         static_cast<uint8_t>(record.command.state) <=
             static_cast<uint8_t>(CommandState::kCancelled) &&
         (record.command.result == protocol::CommandResultCode::kNone ||
          static_cast<uint8_t>(record.command.result) <=
              static_cast<uint8_t>(protocol::CommandResultCode::kInternalError)) &&
         record.checksum == checksum(record);
}

bool CommandStore::begin(CommandBackend& backend, bool& initializedFresh) {
#ifdef ARDUINO
  if (mutex_ == nullptr) mutex_ = xSemaphoreCreateMutex();
  StoreLock lock(mutex_);
  if (!lock) {
    lastError_ = "command state mutex initialization failed";
    return false;
  }
#endif
  backend_ = &backend;
  initializedFresh = false;
  CommandStoreRecord loaded{};
  if (backend.load(loaded) && valid(loaded)) {
    record_ = loaded;
    lastError_ = "command state loaded";
    return true;
  }
  initializedFresh = true;
  initialize(record_);
  if (!backend.save(record_)) {
    lastError_ = "command state initialization write failed";
    return false;
  }
  lastError_ = "command state initialized";
  return true;
}

bool CommandStore::commit(CommandStoreRecord& candidate) {
  if (backend_ == nullptr) return false;
  candidate.checksum = checksum(candidate);
  if (!backend_->save(candidate)) {
    lastError_ = "command state persistence failed";
    return false;
  }
  record_ = candidate;
  return true;
}

bool CommandStore::create(protocol::CommandType type, uint8_t pollIntervalMinutes,
                          uint32_t scheduledMaintenanceUnix,
                          int64_t createdUnixMs, uint32_t& commandId) {
#ifdef ARDUINO
  StoreLock lock(mutex_);
  if (!lock) return false;
#endif
  commandId = 0U;
  if (type == protocol::CommandType::kNone || record_.nextCommandId == UINT32_MAX ||
      record_.command.state == CommandState::kPending ||
      record_.command.state == CommandState::kSent) {
    lastError_ = "a command is already pending or arguments are invalid";
    return false;
  }
  if ((type == protocol::CommandType::kSetPollIntervalMinutes &&
       pollIntervalMinutes == 0U) ||
      (type == protocol::CommandType::kScheduleMaintenanceAt &&
       scheduledMaintenanceUnix == 0U)) {
    lastError_ = "command payload is invalid";
    return false;
  }
  CommandStoreRecord candidate = record_;
  GatewayCommand command{};
  command.commandId = candidate.nextCommandId++;
  command.type = type;
  command.state = CommandState::kPending;
  command.pollIntervalMinutes = pollIntervalMinutes;
  command.scheduledMaintenanceUnix = scheduledMaintenanceUnix;
  command.createdUnixMs = createdUnixMs;
  command.lastSentUnixMs = -1;
  command.result = protocol::CommandResultCode::kNone;
  candidate.command = command;
  if (!commit(candidate)) return false;
  commandId = command.commandId;
  lastError_ = "command created and allocator persisted";
  return true;
}

bool CommandStore::markSent(uint32_t commandId, int64_t sentUnixMs) {
#ifdef ARDUINO
  StoreLock lock(mutex_);
  if (!lock) return false;
#endif
  if (commandId == 0U || commandId != record_.command.commandId ||
      (record_.command.state != CommandState::kPending &&
       record_.command.state != CommandState::kSent)) return false;
  CommandStoreRecord candidate = record_;
  candidate.command.state = CommandState::kSent;
  candidate.command.lastSentUnixMs = sentUnixMs;
  ++candidate.command.sendCount;
  if (!commit(candidate)) return false;
  lastError_ = "command send recorded";
  return true;
}

bool CommandStore::confirm(const protocol::CommandResultPacket& result,
                           bool& duplicate) {
#ifdef ARDUINO
  StoreLock lock(mutex_);
  if (!lock) return false;
#endif
  duplicate = false;
  if (result.commandId == 0U || result.commandId != record_.command.commandId ||
      result.commandType != record_.command.type) {
    lastError_ = "command result does not match current command";
    return false;
  }
  if (record_.command.state == CommandState::kConfirmed) {
    duplicate = true;
    return true;
  }
  if (record_.command.state != CommandState::kPending &&
      record_.command.state != CommandState::kSent) return false;
  CommandStoreRecord candidate = record_;
  candidate.command.state = CommandState::kConfirmed;
  candidate.command.result = result.resultCode;
  candidate.command.effectivePollIntervalMinutes =
      result.effectivePollIntervalMinutes;
  candidate.command.effectiveMaintenanceUnix = result.scheduledMaintenanceUnix;
  if (!commit(candidate)) return false;
  lastError_ = "matching command result confirmed";
  return true;
}

bool CommandStore::cancel(uint32_t commandId) {
#ifdef ARDUINO
  StoreLock lock(mutex_);
  if (!lock) return false;
#endif
  if (commandId == 0U || commandId != record_.command.commandId ||
      (record_.command.state != CommandState::kPending &&
       record_.command.state != CommandState::kSent)) return false;
  CommandStoreRecord candidate = record_;
  candidate.command.state = CommandState::kCancelled;
  if (!commit(candidate)) return false;
  lastError_ = "command cancelled";
  return true;
}

bool CommandStore::pendingForAck(GatewayCommand& command) const {
#ifdef ARDUINO
  StoreLock lock(mutex_);
  if (!lock) return false;
#endif
  if (record_.command.state != CommandState::kPending &&
      record_.command.state != CommandState::kSent) return false;
  command = record_.command;
  return true;
}

GatewayCommand CommandStore::current() const {
#ifdef ARDUINO
  StoreLock lock(mutex_);
  if (!lock) return GatewayCommand{};
#endif
  return record_.command;
}

uint32_t CommandStore::nextCommandId() const {
#ifdef ARDUINO
  StoreLock lock(mutex_);
  if (!lock) return 0U;
#endif
  return record_.nextCommandId;
}

#ifdef ARDUINO
bool NvsCommandBackend::load(CommandStoreRecord& record) {
  Preferences preferences;
  if (!preferences.begin("gathra-cmd", true)) return false;
  const size_t read = preferences.getBytesLength("state") == sizeof(record)
                          ? preferences.getBytes("state", &record, sizeof(record))
                          : 0U;
  preferences.end();
  return read == sizeof(record);
}

bool NvsCommandBackend::save(const CommandStoreRecord& record) {
  Preferences preferences;
  if (!preferences.begin("gathra-cmd", false)) return false;
  const size_t written = preferences.putBytes("state", &record, sizeof(record));
  preferences.end();
  return written == sizeof(record);
}
#endif

const char* commandStateName(CommandState state) {
  switch (state) {
    case CommandState::kNone: return "NONE";
    case CommandState::kPending: return "PENDING";
    case CommandState::kSent: return "SENT";
    case CommandState::kConfirmed: return "CONFIRMED";
    case CommandState::kFailed: return "FAILED";
    case CommandState::kCancelled: return "CANCELLED";
  }
  return "UNKNOWN";
}

}  // namespace gathra::gateway
