#include "protocol.hpp"

#include <string.h>

namespace gathra::gateway::protocol {
namespace {

bool validNodeByte(uint8_t v) {
  return (v >= 'A' && v <= 'Z') || (v >= 'a' && v <= 'z') ||
         (v >= '0' && v <= '9') || v == '-' || v == '_';
}

class Writer {
 public:
  Writer(uint8_t* data, size_t capacity) : data_(data), capacity_(capacity) {}
  bool u8(uint8_t v) {
    if (position_ >= capacity_) return false;
    data_[position_++] = v;
    return true;
  }
  bool u16(uint16_t v) {
    return u8(static_cast<uint8_t>(v >> 8U)) && u8(static_cast<uint8_t>(v));
  }
  bool u32(uint32_t v) {
    return u8(static_cast<uint8_t>(v >> 24U)) &&
           u8(static_cast<uint8_t>(v >> 16U)) &&
           u8(static_cast<uint8_t>(v >> 8U)) && u8(static_cast<uint8_t>(v));
  }
  bool bytes(const uint8_t* data, size_t length) {
    if (data == nullptr || position_ + length > capacity_) return false;
    memcpy(data_ + position_, data, length);
    position_ += length;
    return true;
  }
  size_t size() const { return position_; }

 private:
  uint8_t* data_;
  size_t capacity_;
  size_t position_ = 0;
};

class Reader {
 public:
  Reader(const uint8_t* data, size_t length) : data_(data), length_(length) {}
  bool u8(uint8_t& v) {
    if (position_ >= length_) return false;
    v = data_[position_++];
    return true;
  }
  bool u16(uint16_t& v) {
    uint8_t hi = 0, lo = 0;
    if (!u8(hi) || !u8(lo)) return false;
    v = static_cast<uint16_t>((static_cast<uint16_t>(hi) << 8U) | lo);
    return true;
  }
  bool u32(uint32_t& v) {
    uint8_t b0 = 0, b1 = 0, b2 = 0, b3 = 0;
    if (!u8(b0) || !u8(b1) || !u8(b2) || !u8(b3)) return false;
    v = (static_cast<uint32_t>(b0) << 24U) |
        (static_cast<uint32_t>(b1) << 16U) |
        (static_cast<uint32_t>(b2) << 8U) | b3;
    return true;
  }
  bool bytes(uint8_t* data, size_t length) {
    if (data == nullptr || position_ + length > length_) return false;
    memcpy(data, data_ + position_, length);
    position_ += length;
    return true;
  }
  size_t remaining() const { return length_ - position_; }

 private:
  const uint8_t* data_;
  size_t length_;
  size_t position_ = 0;
};

bool writeHeader(Writer& writer, MessageType type, const char* nodeId) {
  if (!nodeIdValid(nodeId)) return false;
  const size_t length = strnlen(nodeId, build::kNodeIdCapacity);
  return writer.u8(kMagic0) && writer.u8(kMagic1) && writer.u8(kVersion) &&
         writer.u8(static_cast<uint8_t>(type)) &&
         writer.u8(static_cast<uint8_t>(length)) &&
         writer.bytes(reinterpret_cast<const uint8_t*>(nodeId), length);
}

DecodeStatus readHeader(Reader& reader, MessageType expected, char* nodeId) {
  uint8_t m0 = 0, m1 = 0, version = 0, type = 0, length = 0;
  if (!reader.u8(m0) || !reader.u8(m1) || !reader.u8(version) ||
      !reader.u8(type) || !reader.u8(length)) return DecodeStatus::kBufferTooSmall;
  if (m0 != kMagic0 || m1 != kMagic1) return DecodeStatus::kBadMagic;
  if (version != kVersion) return DecodeStatus::kUnsupportedVersion;
  if (type != static_cast<uint8_t>(expected)) return DecodeStatus::kWrongType;
  if (length == 0U || length >= build::kNodeIdCapacity ||
      reader.remaining() < length ||
      !reader.bytes(reinterpret_cast<uint8_t*>(nodeId), length)) {
    return DecodeStatus::kInvalidNodeId;
  }
  nodeId[length] = '\0';
  for (uint8_t i = 0; i < length; ++i) {
    if (!validNodeByte(static_cast<uint8_t>(nodeId[i]))) {
      return DecodeStatus::kInvalidNodeId;
    }
  }
  return DecodeStatus::kOk;
}

bool validBoot(uint8_t v) { return v <= static_cast<uint8_t>(BootReason::kUnknown); }
bool validRtc(uint8_t v) { return v <= static_cast<uint8_t>(RtcState::kI2cError); }
bool validSchedule(uint8_t v) { return v <= static_cast<uint8_t>(ScheduleState::kFailed); }
bool validCommand(uint8_t v) {
  return v <= static_cast<uint8_t>(CommandType::kSetPollIntervalMinutes);
}
bool validResult(uint8_t v, bool allowNone) {
  return v <= static_cast<uint8_t>(CommandResultCode::kInternalError) ||
         (allowNone && v == static_cast<uint8_t>(CommandResultCode::kNone));
}
uint8_t commandPayloadLength(CommandType type) {
  switch (type) {
    case CommandType::kNone:
    case CommandType::kEnterMaintenanceNow: return 0;
    case CommandType::kSetPollIntervalMinutes: return 1;
    case CommandType::kScheduleMaintenanceAt: return 4;
  }
  return UINT8_MAX;
}
bool ackValid(const AckCommandPacket& p) {
  if (!p.timeValid && p.gatewayUnixTime != 0U) return false;
  if (p.commandType == CommandType::kNone) return p.commandId == 0U;
  if (p.commandId == 0U) return false;
  if (p.commandType == CommandType::kSetPollIntervalMinutes) {
    return p.pollIntervalMinutes >= 1U;
  }
  if (p.commandType == CommandType::kScheduleMaintenanceAt) {
    return p.scheduledMaintenanceUnix != 0U;
  }
  return p.commandType == CommandType::kEnterMaintenanceNow;
}

}  // namespace

bool nodeIdValid(const char* nodeId) {
  if (nodeId == nullptr) return false;
  const size_t length = strnlen(nodeId, build::kNodeIdCapacity);
  if (length == 0U || length >= build::kNodeIdCapacity) return false;
  for (size_t i = 0; i < length; ++i) {
    if (!validNodeByte(static_cast<uint8_t>(nodeId[i]))) return false;
  }
  return true;
}

bool encodeTelemetry(const TelemetryPacket& p, uint8_t* output, size_t capacity,
                     size_t& written) {
  written = 0;
  if (output == nullptr || p.pollIntervalMinutes == 0U) return false;
  Writer w(output, capacity);
  if (!writeHeader(w, MessageType::kTelemetry, p.nodeId) ||
      !w.u32(p.persistentSessionId) || !w.u32(p.sequence) ||
      !w.u32(p.medianEchoUs) || !w.u32(p.rawDistanceMm) ||
      !w.u32(p.acceptedDistanceMm) || !w.u16(p.madMm) ||
      !w.u16(static_cast<uint16_t>(p.temperatureCentiC)) ||
      !w.u16(p.humidityCentiPercent) || !w.u16(p.batteryMv) ||
      !w.u8(p.validSamples) || !w.u8(p.totalSamples) ||
      !w.u8(static_cast<uint8_t>(p.filterState)) ||
      !w.u16(p.qualityFlags) || !w.u16(p.healthFlags) ||
      !w.u8(static_cast<uint8_t>(p.bootReason)) ||
      !w.u8(static_cast<uint8_t>(p.rtcState)) || !w.u32(p.rtcUnixTime) ||
      !w.u8(p.pollIntervalMinutes) ||
      !w.u8(static_cast<uint8_t>(p.scheduleState)) ||
      !w.u32(p.scheduledMaintenanceUnix) || !w.u32(p.lastCommandId) ||
      !w.u8(static_cast<uint8_t>(p.lastCommandType)) ||
      !w.u8(static_cast<uint8_t>(p.lastCommandResult))) return false;
  written = w.size();
  return true;
}

DecodeStatus decodeTelemetry(const uint8_t* input, size_t length, TelemetryPacket& p) {
  if (input == nullptr) return DecodeStatus::kBufferTooSmall;
  p = TelemetryPacket{};
  Reader r(input, length);
  DecodeStatus status = readHeader(r, MessageType::kTelemetry, p.nodeId);
  if (status != DecodeStatus::kOk) return status;
  uint16_t temperature = 0;
  uint8_t filter = 0, boot = 0, rtc = 0, schedule = 0, command = 0, result = 0;
  if (!r.u32(p.persistentSessionId) || !r.u32(p.sequence) ||
      !r.u32(p.medianEchoUs) || !r.u32(p.rawDistanceMm) ||
      !r.u32(p.acceptedDistanceMm) || !r.u16(p.madMm) ||
      !r.u16(temperature) || !r.u16(p.humidityCentiPercent) ||
      !r.u16(p.batteryMv) || !r.u8(p.validSamples) || !r.u8(p.totalSamples) ||
      !r.u8(filter) || !r.u16(p.qualityFlags) || !r.u16(p.healthFlags) ||
      !r.u8(boot) || !r.u8(rtc) || !r.u32(p.rtcUnixTime) ||
      !r.u8(p.pollIntervalMinutes) || !r.u8(schedule) ||
      !r.u32(p.scheduledMaintenanceUnix) || !r.u32(p.lastCommandId) ||
      !r.u8(command) || !r.u8(result)) return DecodeStatus::kBufferTooSmall;
  if (filter > static_cast<uint8_t>(FilterState::kInvalid) || !validBoot(boot) ||
      !validRtc(rtc) || !validSchedule(schedule) || !validCommand(command) ||
      !validResult(result, true) || p.pollIntervalMinutes == 0U) {
    return DecodeStatus::kInvalidEnum;
  }
  if ((rtc != static_cast<uint8_t>(RtcState::kValid) && p.rtcUnixTime != 0U) ||
      (schedule == static_cast<uint8_t>(ScheduleState::kNone) &&
       p.scheduledMaintenanceUnix != 0U) ||
      (command == static_cast<uint8_t>(CommandType::kNone) && p.lastCommandId != 0U)) {
    return DecodeStatus::kInvalidFlags;
  }
  p.temperatureCentiC = static_cast<int16_t>(temperature);
  p.filterState = static_cast<FilterState>(filter);
  p.bootReason = static_cast<BootReason>(boot);
  p.rtcState = static_cast<RtcState>(rtc);
  p.scheduleState = static_cast<ScheduleState>(schedule);
  p.lastCommandType = static_cast<CommandType>(command);
  p.lastCommandResult = static_cast<CommandResultCode>(result);
  return r.remaining() == 0U ? DecodeStatus::kOk : DecodeStatus::kTrailingData;
}

bool encodeAckCommand(const AckCommandPacket& p, uint8_t* output,
                      size_t capacity, size_t& written) {
  written = 0;
  if (output == nullptr || !ackValid(p)) return false;
  Writer w(output, capacity);
  const uint8_t payloadLength = commandPayloadLength(p.commandType);
  if (!writeHeader(w, MessageType::kAckCommand, p.nodeId) ||
      !w.u32(p.persistentSessionId) || !w.u32(p.sequence) ||
      !w.u32(p.gatewayUnixTime) || !w.u8(p.timeValid ? kAckTimeValid : 0U) ||
      !w.u32(p.commandId) || !w.u8(static_cast<uint8_t>(p.commandType)) ||
      !w.u8(payloadLength)) return false;
  if (p.commandType == CommandType::kSetPollIntervalMinutes &&
      !w.u8(p.pollIntervalMinutes)) return false;
  if (p.commandType == CommandType::kScheduleMaintenanceAt &&
      !w.u32(p.scheduledMaintenanceUnix)) return false;
  written = w.size();
  return true;
}

DecodeStatus decodeAckCommand(const uint8_t* input, size_t length,
                              AckCommandPacket& p) {
  if (input == nullptr) return DecodeStatus::kBufferTooSmall;
  p = AckCommandPacket{};
  Reader r(input, length);
  DecodeStatus status = readHeader(r, MessageType::kAckCommand, p.nodeId);
  if (status != DecodeStatus::kOk) return status;
  uint8_t flags = 0, command = 0, payloadLength = 0;
  if (!r.u32(p.persistentSessionId) || !r.u32(p.sequence) ||
      !r.u32(p.gatewayUnixTime) || !r.u8(flags) || !r.u32(p.commandId) ||
      !r.u8(command) || !r.u8(payloadLength)) return DecodeStatus::kBufferTooSmall;
  if ((flags & ~kAckTimeValid) != 0U) return DecodeStatus::kInvalidFlags;
  if (!validCommand(command)) return DecodeStatus::kInvalidCommand;
  p.timeValid = (flags & kAckTimeValid) != 0U;
  p.commandType = static_cast<CommandType>(command);
  if (payloadLength != commandPayloadLength(p.commandType)) {
    return DecodeStatus::kInvalidCommand;
  }
  if (p.commandType == CommandType::kSetPollIntervalMinutes &&
      !r.u8(p.pollIntervalMinutes)) return DecodeStatus::kBufferTooSmall;
  if (p.commandType == CommandType::kScheduleMaintenanceAt &&
      !r.u32(p.scheduledMaintenanceUnix)) return DecodeStatus::kBufferTooSmall;
  if (!ackValid(p)) return DecodeStatus::kInvalidCommand;
  return r.remaining() == 0U ? DecodeStatus::kOk : DecodeStatus::kTrailingData;
}

bool encodeCommandResult(const CommandResultPacket& p, uint8_t* output,
                         size_t capacity, size_t& written) {
  written = 0;
  if (output == nullptr || p.commandId == 0U ||
      p.commandType == CommandType::kNone ||
      !validResult(static_cast<uint8_t>(p.resultCode), false)) return false;
  Writer w(output, capacity);
  if (!writeHeader(w, MessageType::kCommandResult, p.nodeId) ||
      !w.u32(p.persistentSessionId) || !w.u32(p.commandId) ||
      !w.u8(static_cast<uint8_t>(p.commandType)) ||
      !w.u8(static_cast<uint8_t>(p.resultCode)) ||
      !w.u8(p.effectivePollIntervalMinutes) ||
      !w.u32(p.scheduledMaintenanceUnix)) return false;
  written = w.size();
  return true;
}

DecodeStatus decodeCommandResult(const uint8_t* input, size_t length,
                                 CommandResultPacket& p) {
  if (input == nullptr) return DecodeStatus::kBufferTooSmall;
  p = CommandResultPacket{};
  Reader r(input, length);
  DecodeStatus status = readHeader(r, MessageType::kCommandResult, p.nodeId);
  if (status != DecodeStatus::kOk) return status;
  uint8_t command = 0, result = 0;
  if (!r.u32(p.persistentSessionId) || !r.u32(p.commandId) ||
      !r.u8(command) || !r.u8(result) ||
      !r.u8(p.effectivePollIntervalMinutes) ||
      !r.u32(p.scheduledMaintenanceUnix)) return DecodeStatus::kBufferTooSmall;
  if (!validCommand(command) || command == 0U || !validResult(result, false) ||
      p.commandId == 0U) return DecodeStatus::kInvalidCommand;
  p.commandType = static_cast<CommandType>(command);
  p.resultCode = static_cast<CommandResultCode>(result);
  return r.remaining() == 0U ? DecodeStatus::kOk : DecodeStatus::kTrailingData;
}

AckCommandPacket makeAck(const TelemetryPacket& telemetry, uint32_t gatewayUnix,
                         bool timeValid) {
  AckCommandPacket ack{};
  strncpy(ack.nodeId, telemetry.nodeId, sizeof(ack.nodeId) - 1U);
  ack.persistentSessionId = telemetry.persistentSessionId;
  ack.sequence = telemetry.sequence;
  ack.gatewayUnixTime = timeValid ? gatewayUnix : 0U;
  ack.timeValid = timeValid;
  return ack;
}

bool ackMatches(const AckCommandPacket& ack, const TelemetryPacket& telemetry) {
  return strncmp(ack.nodeId, telemetry.nodeId, build::kNodeIdCapacity) == 0 &&
         ack.persistentSessionId == telemetry.persistentSessionId &&
         ack.sequence == telemetry.sequence;
}

const char* decodeStatusName(DecodeStatus status) {
  switch (status) {
    case DecodeStatus::kOk: return "OK";
    case DecodeStatus::kBufferTooSmall: return "BUFFER_TOO_SMALL";
    case DecodeStatus::kBadMagic: return "BAD_MAGIC";
    case DecodeStatus::kUnsupportedVersion: return "UNSUPPORTED_VERSION";
    case DecodeStatus::kWrongType: return "WRONG_TYPE";
    case DecodeStatus::kInvalidNodeId: return "INVALID_NODE_ID";
    case DecodeStatus::kInvalidEnum: return "INVALID_ENUM";
    case DecodeStatus::kInvalidFlags: return "INVALID_FLAGS";
    case DecodeStatus::kInvalidCommand: return "INVALID_COMMAND";
    case DecodeStatus::kTrailingData: return "TRAILING_DATA";
  }
  return "UNKNOWN";
}

const char* filterStateName(FilterState state) {
  switch (state) {
    case FilterState::kStable: return "STABLE";
    case FilterState::kAccepted: return "ACCEPTED";
    case FilterState::kVerifyRise: return "VERIFY_RISE";
    case FilterState::kVerifyFall: return "VERIFY_FALL";
    case FilterState::kTransientRejected: return "TRANSIENT_REJECTED";
    case FilterState::kChangeConfirmed: return "CHANGE_CONFIRMED";
    case FilterState::kUncertain: return "UNCERTAIN";
    case FilterState::kInvalid: return "INVALID";
  }
  return "UNKNOWN";
}

const char* bootReasonName(BootReason reason) {
  switch (reason) {
    case BootReason::kRtcTimer: return "RTC_TIMER";
    case BootReason::kRtcScheduledMaintenance: return "RTC_SCHEDULED_MAINTENANCE";
    case BootReason::kManualButton: return "MANUAL_BUTTON";
    case BootReason::kMaintenanceReboot: return "MAINTENANCE_REBOOT";
    case BootReason::kOtaReboot: return "OTA_REBOOT";
    case BootReason::kUnknown: return "UNKNOWN";
  }
  return "UNKNOWN";
}

const char* rtcStateName(RtcState state) {
  switch (state) {
    case RtcState::kValid: return "VALID";
    case RtcState::kInvalidVl: return "INVALID_VL";
    case RtcState::kUninitialized: return "UNINITIALIZED";
    case RtcState::kI2cError: return "I2C_ERROR";
  }
  return "I2C_ERROR";
}

const char* scheduleStateName(ScheduleState state) {
  switch (state) {
    case ScheduleState::kNone: return "NONE";
    case ScheduleState::kPending: return "PENDING";
    case ScheduleState::kCompleted: return "COMPLETED";
    case ScheduleState::kFailed: return "FAILED";
  }
  return "FAILED";
}

const char* commandTypeName(CommandType type) {
  switch (type) {
    case CommandType::kNone: return "NONE";
    case CommandType::kEnterMaintenanceNow: return "ENTER_MAINTENANCE_NOW";
    case CommandType::kScheduleMaintenanceAt: return "SCHEDULE_MAINTENANCE_AT";
    case CommandType::kSetPollIntervalMinutes: return "SET_POLL_INTERVAL_MINUTES";
  }
  return "UNKNOWN";
}

const char* commandResultName(CommandResultCode result) {
  switch (result) {
    case CommandResultCode::kApplied: return "APPLIED";
    case CommandResultCode::kAlreadyApplied: return "ALREADY_APPLIED";
    case CommandResultCode::kInvalidArgument: return "INVALID_ARGUMENT";
    case CommandResultCode::kRtcUnavailable: return "RTC_UNAVAILABLE";
    case CommandResultCode::kRtcTimeUntrusted: return "RTC_TIME_UNTRUSTED";
    case CommandResultCode::kScheduleUnrepresentable: return "SCHEDULE_UNREPRESENTABLE";
    case CommandResultCode::kStorageError: return "STORAGE_ERROR";
    case CommandResultCode::kInternalError: return "INTERNAL_ERROR";
    case CommandResultCode::kNone: return "NONE";
  }
  return "UNKNOWN";
}

}  // namespace gathra::gateway::protocol
