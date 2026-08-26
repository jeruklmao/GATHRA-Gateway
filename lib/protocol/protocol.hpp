#pragma once

#include <stddef.h>
#include <stdint.h>

#include "build_config.hpp"

namespace gathra::gateway::protocol {

inline constexpr uint8_t kMagic0 = 0x47;
inline constexpr uint8_t kMagic1 = 0x54;
inline constexpr uint8_t kVersion = 3;
inline constexpr uint8_t kAckTimeValid = 1U << 0;
inline constexpr uint32_t kDistanceUnavailable = UINT32_MAX;
inline constexpr int16_t kTemperatureUnavailable = INT16_MIN;
inline constexpr uint16_t kHumidityUnavailable = UINT16_MAX;
inline constexpr size_t kCommonHeaderFixedBytes = 5U;
inline constexpr size_t kReferenceDistancePayloadOffset = 53U;
inline constexpr size_t kReferenceDistanceBytes = 4U;
inline constexpr size_t kTelemetryPayloadBytes =
    kReferenceDistancePayloadOffset + kReferenceDistanceBytes;
inline constexpr size_t kAckCommandFixedPayloadBytes = 19U;
inline constexpr size_t kCommandResultPayloadBytes = 15U;
inline constexpr size_t kMaximumTelemetryPacketBytes =
    kCommonHeaderFixedBytes + (build::kNodeIdCapacity - 1U) +
    kTelemetryPayloadBytes;

static_assert(kTelemetryPayloadBytes == 57U,
              "Protocol v3 TELEMETRY payload size must remain 57 bytes");
static_assert(kMaximumTelemetryPacketBytes <= build::kRadioPacketCapacity,
              "Protocol v3 TELEMETRY exceeds radio/queue capacity");

enum class MessageType : uint8_t {
  kTelemetry = 0x01,
  kAckCommand = 0x02,
  kCommandResult = 0x03,
};

enum class FilterState : uint8_t {
  kStable = 0, kAccepted = 1, kVerifyRise = 2, kVerifyFall = 3,
  kTransientRejected = 4, kChangeConfirmed = 5, kUncertain = 6, kInvalid = 7,
};

enum class BootReason : uint8_t {
  kRtcTimer = 0, kRtcScheduledMaintenance = 1, kManualButton = 2,
  kMaintenanceReboot = 3, kOtaReboot = 4, kUnknown = 5,
};

enum class RtcState : uint8_t {
  kValid = 0, kInvalidVl = 1, kUninitialized = 2, kI2cError = 3,
};

enum class ScheduleState : uint8_t {
  kNone = 0, kPending = 1, kCompleted = 2, kFailed = 3,
};

enum class CommandType : uint8_t {
  kNone = 0, kEnterMaintenanceNow = 1, kScheduleMaintenanceAt = 2,
  kSetPollIntervalMinutes = 3,
};

enum class CommandResultCode : uint8_t {
  kApplied = 0, kAlreadyApplied = 1, kInvalidArgument = 2,
  kRtcUnavailable = 3, kRtcTimeUntrusted = 4, kScheduleUnrepresentable = 5,
  kStorageError = 6, kInternalError = 7, kNone = 0xFF,
};

enum class DecodeStatus : uint8_t {
  kOk, kBufferTooSmall, kBadMagic, kUnsupportedVersion, kWrongType,
  kInvalidNodeId, kInvalidEnum, kInvalidFlags, kInvalidCommand, kTrailingData,
};

struct TelemetryPacket {
  char nodeId[build::kNodeIdCapacity]{};
  uint32_t persistentSessionId = 0;
  uint32_t sequence = 0;
  uint32_t medianEchoUs = 0;
  uint32_t rawDistanceMm = kDistanceUnavailable;
  uint32_t acceptedDistanceMm = kDistanceUnavailable;
  uint16_t madMm = 0;
  int16_t temperatureCentiC = kTemperatureUnavailable;
  uint16_t humidityCentiPercent = kHumidityUnavailable;
  uint16_t batteryMv = 0;
  uint8_t validSamples = 0;
  uint8_t totalSamples = 0;
  FilterState filterState = FilterState::kInvalid;
  uint16_t qualityFlags = 0;
  uint16_t healthFlags = 0;
  BootReason bootReason = BootReason::kUnknown;
  RtcState rtcState = RtcState::kUninitialized;
  uint32_t rtcUnixTime = 0;
  uint8_t pollIntervalMinutes = 10;
  ScheduleState scheduleState = ScheduleState::kNone;
  uint32_t scheduledMaintenanceUnix = 0;
  uint32_t lastCommandId = 0;
  CommandType lastCommandType = CommandType::kNone;
  CommandResultCode lastCommandResult = CommandResultCode::kNone;
  uint32_t referenceDistanceMm = 0;
};

struct AckCommandPacket {
  char nodeId[build::kNodeIdCapacity]{};
  uint32_t persistentSessionId = 0;
  uint32_t sequence = 0;
  uint32_t gatewayUnixTime = 0;
  bool timeValid = false;
  uint32_t commandId = 0;
  CommandType commandType = CommandType::kNone;
  uint8_t pollIntervalMinutes = 0;
  uint32_t scheduledMaintenanceUnix = 0;
};

struct CommandResultPacket {
  char nodeId[build::kNodeIdCapacity]{};
  uint32_t persistentSessionId = 0;
  uint32_t commandId = 0;
  CommandType commandType = CommandType::kNone;
  CommandResultCode resultCode = CommandResultCode::kInternalError;
  uint8_t effectivePollIntervalMinutes = 0;
  uint32_t scheduledMaintenanceUnix = 0;
};

bool nodeIdValid(const char* nodeId);
bool encodeTelemetry(const TelemetryPacket& packet, uint8_t* output,
                     size_t capacity, size_t& written);
DecodeStatus decodeTelemetry(const uint8_t* input, size_t length,
                             TelemetryPacket& packet);
bool encodeAckCommand(const AckCommandPacket& packet, uint8_t* output,
                      size_t capacity, size_t& written);
DecodeStatus decodeAckCommand(const uint8_t* input, size_t length,
                              AckCommandPacket& packet);
bool encodeCommandResult(const CommandResultPacket& packet, uint8_t* output,
                         size_t capacity, size_t& written);
DecodeStatus decodeCommandResult(const uint8_t* input, size_t length,
                                 CommandResultPacket& packet);
AckCommandPacket makeAck(const TelemetryPacket& telemetry, uint32_t gatewayUnix,
                         bool timeValid);
bool ackMatches(const AckCommandPacket& ack, const TelemetryPacket& telemetry);
const char* decodeStatusName(DecodeStatus status);
const char* filterStateName(FilterState state);
const char* bootReasonName(BootReason reason);
const char* rtcStateName(RtcState state);
const char* scheduleStateName(ScheduleState state);
const char* commandTypeName(CommandType type);
const char* commandResultName(CommandResultCode result);

}  // namespace gathra::gateway::protocol
