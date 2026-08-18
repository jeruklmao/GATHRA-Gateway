#include "protocol.hpp"

#include <string.h>

namespace gathra::gateway::protocol {
namespace {

bool nodeIdByteValid(uint8_t value) {
  return (value >= static_cast<uint8_t>('A') &&
          value <= static_cast<uint8_t>('Z')) ||
         (value >= static_cast<uint8_t>('a') &&
          value <= static_cast<uint8_t>('z')) ||
         (value >= static_cast<uint8_t>('0') &&
          value <= static_cast<uint8_t>('9')) ||
         value == static_cast<uint8_t>('-') ||
         value == static_cast<uint8_t>('_');
}

class Reader {
 public:
  Reader(const uint8_t* data, size_t length) : data_(data), length_(length) {}
  bool u8(uint8_t& value) {
    if (position_ >= length_) return false;
    value = data_[position_++];
    return true;
  }
  bool u16(uint16_t& value) {
    uint8_t hi = 0, lo = 0;
    if (!u8(hi) || !u8(lo)) return false;
    value = static_cast<uint16_t>((static_cast<uint16_t>(hi) << 8U) | lo);
    return true;
  }
  bool u32(uint32_t& value) {
    uint8_t b0 = 0, b1 = 0, b2 = 0, b3 = 0;
    if (!u8(b0) || !u8(b1) || !u8(b2) || !u8(b3)) return false;
    value = (static_cast<uint32_t>(b0) << 24U) |
            (static_cast<uint32_t>(b1) << 16U) |
            (static_cast<uint32_t>(b2) << 8U) | b3;
    return true;
  }
  bool bytes(uint8_t* output, size_t length) {
    if (output == nullptr || position_ + length > length_) return false;
    memcpy(output, data_ + position_, length);
    position_ += length;
    return true;
  }
  size_t remaining() const { return length_ - position_; }

 private:
  const uint8_t* data_;
  size_t length_;
  size_t position_ = 0;
};

class Writer {
 public:
  Writer(uint8_t* data, size_t capacity) : data_(data), capacity_(capacity) {}
  bool u8(uint8_t value) {
    if (position_ >= capacity_) return false;
    data_[position_++] = value;
    return true;
  }
  bool u32(uint32_t value) {
    return u8(static_cast<uint8_t>(value >> 24U)) &&
           u8(static_cast<uint8_t>(value >> 16U)) &&
           u8(static_cast<uint8_t>(value >> 8U)) && u8(static_cast<uint8_t>(value));
  }
  bool bytes(const uint8_t* input, size_t length) {
    if (input == nullptr || position_ + length > capacity_) return false;
    memcpy(data_ + position_, input, length);
    position_ += length;
    return true;
  }
  size_t size() const { return position_; }

 private:
  uint8_t* data_;
  size_t capacity_;
  size_t position_ = 0;
};

DecodeStatus readHeader(Reader& reader, MessageType expected, char* nodeId) {
  uint8_t magic0 = 0, magic1 = 0, version = 0, type = 0, length = 0;
  if (!reader.u8(magic0) || !reader.u8(magic1) || !reader.u8(version) ||
      !reader.u8(type) || !reader.u8(length)) {
    return DecodeStatus::kBufferTooSmall;
  }
  if (magic0 != kMagic0 || magic1 != kMagic1) return DecodeStatus::kBadMagic;
  if (version != kVersion) return DecodeStatus::kUnsupportedVersion;
  if (type != static_cast<uint8_t>(expected)) return DecodeStatus::kWrongType;
  if (length == 0U || length >= build::kNodeIdCapacity ||
      reader.remaining() < length || !reader.bytes(reinterpret_cast<uint8_t*>(nodeId), length)) {
    return DecodeStatus::kInvalidNodeId;
  }
  nodeId[length] = '\0';
  // Validate all bytes covered by the on-air length. In particular, an
  // embedded NUL must not silently shorten the C string and become accepted.
  for (uint8_t index = 0; index < length; ++index) {
    if (!nodeIdByteValid(static_cast<uint8_t>(nodeId[index]))) {
      return DecodeStatus::kInvalidNodeId;
    }
  }
  return DecodeStatus::kOk;
}

bool writeHeader(Writer& writer, MessageType type, const char* nodeId) {
  if (!nodeIdValid(nodeId)) return false;
  const size_t length = strnlen(nodeId, build::kNodeIdCapacity);
  return writer.u8(kMagic0) && writer.u8(kMagic1) && writer.u8(kVersion) &&
         writer.u8(static_cast<uint8_t>(type)) && writer.u8(static_cast<uint8_t>(length)) &&
         writer.bytes(reinterpret_cast<const uint8_t*>(nodeId), length);
}

}  // namespace

bool nodeIdValid(const char* nodeId) {
  if (nodeId == nullptr) return false;
  const size_t length = strnlen(nodeId, build::kNodeIdCapacity);
  if (length == 0U || length >= build::kNodeIdCapacity) return false;
  for (size_t i = 0; i < length; ++i) {
    if (!nodeIdByteValid(static_cast<uint8_t>(nodeId[i]))) return false;
  }
  return true;
}

DecodeStatus decodeTelemetry(const uint8_t* input, size_t length, TelemetryPacket& p) {
  if (input == nullptr) return DecodeStatus::kBufferTooSmall;
  p = TelemetryPacket{};
  Reader reader(input, length);
  DecodeStatus status = readHeader(reader, MessageType::kTelemetry, p.nodeId);
  if (status != DecodeStatus::kOk) return status;
  uint16_t temperature = 0;
  uint8_t filter = 0;
  if (!reader.u32(p.bootSessionId) || !reader.u32(p.sequence) ||
      !reader.u32(p.medianEchoUs) || !reader.u32(p.rawDistanceMm) ||
      !reader.u32(p.acceptedDistanceMm) || !reader.u16(p.madMm) ||
      !reader.u16(temperature) || !reader.u16(p.humidityCentiPercent) ||
      !reader.u16(p.batteryMv) || !reader.u8(p.validSamples) ||
      !reader.u8(p.totalSamples) || !reader.u8(filter) ||
      !reader.u16(p.qualityFlags) || !reader.u16(p.healthFlags)) {
    return DecodeStatus::kBufferTooSmall;
  }
  if (filter > static_cast<uint8_t>(FilterState::kInvalid)) {
    return DecodeStatus::kInvalidFilterState;
  }
  p.temperatureCentiC = static_cast<int16_t>(temperature);
  p.filterState = static_cast<FilterState>(filter);
  return reader.remaining() == 0U ? DecodeStatus::kOk : DecodeStatus::kTrailingData;
}

bool encodeAck(const AckPacket& p, uint8_t* output, size_t capacity, size_t& written) {
  written = 0;
  if (output == nullptr) return false;
  Writer writer(output, capacity);
  if (!writeHeader(writer, MessageType::kAck, p.nodeId) ||
      !writer.u32(p.bootSessionId) || !writer.u32(p.sequence)) {
    return false;
  }
  written = writer.size();
  return true;
}

DecodeStatus decodeAck(const uint8_t* input, size_t length, AckPacket& p) {
  if (input == nullptr) return DecodeStatus::kBufferTooSmall;
  p = AckPacket{};
  Reader reader(input, length);
  DecodeStatus status = readHeader(reader, MessageType::kAck, p.nodeId);
  if (status != DecodeStatus::kOk) return status;
  if (!reader.u32(p.bootSessionId) || !reader.u32(p.sequence)) {
    return DecodeStatus::kBufferTooSmall;
  }
  return reader.remaining() == 0U ? DecodeStatus::kOk : DecodeStatus::kTrailingData;
}

AckPacket makeAck(const TelemetryPacket& telemetry) {
  AckPacket ack{};
  strncpy(ack.nodeId, telemetry.nodeId, sizeof(ack.nodeId) - 1U);
  ack.bootSessionId = telemetry.bootSessionId;
  ack.sequence = telemetry.sequence;
  return ack;
}

bool ackMatches(const AckPacket& ack, const TelemetryPacket& telemetry) {
  return strncmp(ack.nodeId, telemetry.nodeId, build::kNodeIdCapacity) == 0 &&
         ack.bootSessionId == telemetry.bootSessionId && ack.sequence == telemetry.sequence;
}

const char* decodeStatusName(DecodeStatus status) {
  switch (status) {
    case DecodeStatus::kOk: return "OK";
    case DecodeStatus::kBufferTooSmall: return "BUFFER_TOO_SMALL";
    case DecodeStatus::kBadMagic: return "BAD_MAGIC";
    case DecodeStatus::kUnsupportedVersion: return "UNSUPPORTED_VERSION";
    case DecodeStatus::kWrongType: return "WRONG_TYPE";
    case DecodeStatus::kInvalidNodeId: return "INVALID_NODE_ID";
    case DecodeStatus::kInvalidFilterState: return "INVALID_FILTER_STATE";
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

}  // namespace gathra::gateway::protocol
