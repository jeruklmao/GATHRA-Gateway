#pragma once

#include <stddef.h>
#include <stdint.h>

#include "protocol.hpp"

#ifdef ARDUINO
#include <Preferences.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#endif

namespace gathra::gateway {

inline constexpr uint32_t kCommandStoreMagic = 0x47574332U;  // GWC2
inline constexpr uint16_t kCommandStoreSchema = 1U;

enum class CommandState : uint8_t {
  kNone = 0,
  kPending = 1,
  kSent = 2,
  kConfirmed = 3,
  kFailed = 4,
  kCancelled = 5,
};

struct GatewayCommand {
  uint32_t commandId = 0;
  protocol::CommandType type = protocol::CommandType::kNone;
  CommandState state = CommandState::kNone;
  uint8_t pollIntervalMinutes = 0;
  uint32_t scheduledMaintenanceUnix = 0;
  int64_t createdUnixMs = -1;
  int64_t lastSentUnixMs = -1;
  uint32_t sendCount = 0;
  protocol::CommandResultCode result = protocol::CommandResultCode::kNone;
  uint8_t effectivePollIntervalMinutes = 0;
  uint32_t effectiveMaintenanceUnix = 0;
};

struct CommandStoreRecord {
  uint32_t magic = kCommandStoreMagic;
  uint16_t schemaVersion = kCommandStoreSchema;
  uint16_t structureSize = 0;
  uint32_t nextCommandId = 1;
  GatewayCommand command{};
  uint32_t checksum = 0;
};

class CommandBackend {
 public:
  virtual ~CommandBackend() = default;
  virtual bool load(CommandStoreRecord& record) = 0;
  virtual bool save(const CommandStoreRecord& record) = 0;
};

#ifdef ARDUINO
class NvsCommandBackend final : public CommandBackend {
 public:
  bool load(CommandStoreRecord& record) override;
  bool save(const CommandStoreRecord& record) override;
};
#endif

class CommandStore {
 public:
  bool begin(CommandBackend& backend, bool& initializedFresh);
  bool create(protocol::CommandType type, uint8_t pollIntervalMinutes,
              uint32_t scheduledMaintenanceUnix, int64_t createdUnixMs,
              uint32_t& commandId);
  bool markSent(uint32_t commandId, int64_t sentUnixMs);
  bool confirm(const protocol::CommandResultPacket& result, bool& duplicate);
  bool cancel(uint32_t commandId);
  bool pendingForAck(GatewayCommand& command) const;
  GatewayCommand current() const;
  uint32_t nextCommandId() const;
  const char* lastError() const { return lastError_; }

  static void initialize(CommandStoreRecord& record);
  static uint32_t checksum(const CommandStoreRecord& record);
  static bool valid(const CommandStoreRecord& record);

 private:
  bool commit(CommandStoreRecord& candidate);
  CommandBackend* backend_ = nullptr;
  CommandStoreRecord record_{};
  const char* lastError_ = "not initialized";
#ifdef ARDUINO
  mutable SemaphoreHandle_t mutex_ = nullptr;
#endif
};

const char* commandStateName(CommandState state);

}  // namespace gathra::gateway
