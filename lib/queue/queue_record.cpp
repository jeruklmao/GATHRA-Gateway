#include "queue_record.hpp"

#include <string.h>

namespace gathra::gateway {
namespace {

class Writer {
 public:
  explicit Writer(std::vector<uint8_t>& output) : output_(output) {}
  void u8(uint8_t value) { output_.push_back(value); }
  void u16(uint16_t value) {
    u8(static_cast<uint8_t>(value >> 8U));
    u8(static_cast<uint8_t>(value));
  }
  void u32(uint32_t value) {
    u8(static_cast<uint8_t>(value >> 24U));
    u8(static_cast<uint8_t>(value >> 16U));
    u8(static_cast<uint8_t>(value >> 8U));
    u8(static_cast<uint8_t>(value));
  }
  void u64(uint64_t value) {
    u32(static_cast<uint32_t>(value >> 32U));
    u32(static_cast<uint32_t>(value));
  }
  void bytes(const uint8_t* input, size_t length) {
    output_.insert(output_.end(), input, input + length);
  }

 private:
  std::vector<uint8_t>& output_;
};

class Reader {
 public:
  Reader(const uint8_t* input, size_t length) : input_(input), length_(length) {}
  bool u8(uint8_t& value) {
    if (position_ >= length_) return false;
    value = input_[position_++];
    return true;
  }
  bool u16(uint16_t& value) {
    uint8_t a = 0, b = 0;
    if (!u8(a) || !u8(b)) return false;
    value = static_cast<uint16_t>((static_cast<uint16_t>(a) << 8U) | b);
    return true;
  }
  bool u32(uint32_t& value) {
    uint8_t a = 0, b = 0, c = 0, d = 0;
    if (!u8(a) || !u8(b) || !u8(c) || !u8(d)) return false;
    value = (static_cast<uint32_t>(a) << 24U) |
            (static_cast<uint32_t>(b) << 16U) |
            (static_cast<uint32_t>(c) << 8U) | d;
    return true;
  }
  bool u64(uint64_t& value) {
    uint32_t hi = 0, lo = 0;
    if (!u32(hi) || !u32(lo)) return false;
    value = (static_cast<uint64_t>(hi) << 32U) | lo;
    return true;
  }
  bool bytes(uint8_t* output, size_t length) {
    if (output == nullptr || position_ + length > length_) return false;
    memcpy(output, input_ + position_, length);
    position_ += length;
    return true;
  }
  size_t remaining() const { return length_ - position_; }

 private:
  const uint8_t* input_;
  size_t length_;
  size_t position_ = 0;
};

uint32_t floatBits(float value) {
  uint32_t bits = 0;
  memcpy(&bits, &value, sizeof(bits));
  return bits;
}

float bitsFloat(uint32_t bits) {
  float value = 0.0F;
  memcpy(&value, &bits, sizeof(value));
  return value;
}

}  // namespace

bool QueueRecordCodec::encode(const QueueRecord& record,
                              std::vector<uint8_t>& output) {
  output.clear();
  if (record.recordId == 0U || record.payloadLength == 0U ||
      record.payloadLength > build::kRadioPacketCapacity ||
      record.reception.packetLength != record.payloadLength ||
      (record.reception.gatewayTimeTrusted &&
       record.reception.gatewayReceivedUnixMs < 0) ||
      (!record.reception.gatewayTimeTrusted &&
       record.reception.gatewayReceivedUnixMs != -1)) {
    return false;
  }
  output.reserve(kFixedBytesWithoutPayload + record.payloadLength);
  Writer writer(output);
  writer.u32(build::kQueueMagic);
  writer.u16(build::kQueueRecordVersion);
  writer.u16(static_cast<uint16_t>(kFixedBytesWithoutPayload + record.payloadLength));
  writer.u64(record.recordId);
  writer.u8(record.reception.gatewayTimeTrusted ? 1U : 0U);
  writer.u8(record.payloadLength);
  writer.u16(record.reception.packetLength);
  writer.u32(record.reception.gatewayBootSessionId);
  writer.u64(static_cast<uint64_t>(record.reception.gatewayReceivedUnixMs));
  writer.u64(record.reception.gatewayUptimeMs);
  writer.u32(floatBits(record.reception.rssiDbm));
  writer.u32(floatBits(record.reception.snrDb));
  writer.u32(static_cast<uint32_t>(record.reception.frequencyErrorHz));
  writer.bytes(record.rawPayload, record.payloadLength);
  writer.u32(crc32(output.data(), output.size()));
  return output.size() == kFixedBytesWithoutPayload + record.payloadLength;
}

bool QueueRecordCodec::decode(const uint8_t* input, size_t length,
                              QueueRecord& record) {
  record = {};
  if (input == nullptr || length < kFixedBytesWithoutPayload ||
      length > kMaximumEncodedBytes) {
    return false;
  }
  const uint32_t storedCrc = (static_cast<uint32_t>(input[length - 4U]) << 24U) |
                             (static_cast<uint32_t>(input[length - 3U]) << 16U) |
                             (static_cast<uint32_t>(input[length - 2U]) << 8U) |
                             input[length - 1U];
  if (storedCrc != crc32(input, length - 4U)) return false;
  Reader reader(input, length - 4U);
  uint32_t magic = 0, boot = 0, rssi = 0, snr = 0, frequencyError = 0;
  uint16_t version = 0, encodedLength = 0, packetLength = 0;
  uint8_t flags = 0, payloadLength = 0;
  uint64_t receivedAtBits = 0;
  if (!reader.u32(magic) || !reader.u16(version) || !reader.u16(encodedLength) ||
      !reader.u64(record.recordId) || !reader.u8(flags) ||
      !reader.u8(payloadLength) || !reader.u16(packetLength) ||
      !reader.u32(boot) || !reader.u64(receivedAtBits) ||
      !reader.u64(record.reception.gatewayUptimeMs) || !reader.u32(rssi) ||
      !reader.u32(snr) || !reader.u32(frequencyError)) {
    return false;
  }
  if (magic != build::kQueueMagic || version != build::kQueueRecordVersion ||
      encodedLength != length || record.recordId == 0U || flags > 1U ||
      payloadLength == 0U || payloadLength > build::kRadioPacketCapacity ||
      packetLength != payloadLength || reader.remaining() != payloadLength) {
    return false;
  }
  record.payloadLength = payloadLength;
  record.reception.packetLength = packetLength;
  record.reception.gatewayBootSessionId = boot;
  record.reception.gatewayTimeTrusted = flags == 1U;
  record.reception.gatewayReceivedUnixMs = static_cast<int64_t>(receivedAtBits);
  record.reception.rssiDbm = bitsFloat(rssi);
  record.reception.snrDb = bitsFloat(snr);
  record.reception.frequencyErrorHz = static_cast<int32_t>(frequencyError);
  if ((record.reception.gatewayTimeTrusted &&
       record.reception.gatewayReceivedUnixMs < 0) ||
      (!record.reception.gatewayTimeTrusted &&
       record.reception.gatewayReceivedUnixMs != -1)) {
    return false;
  }
  return reader.bytes(record.rawPayload, payloadLength) && reader.remaining() == 0U;
}

uint32_t QueueRecordCodec::crc32(const uint8_t* input, size_t length) {
  uint32_t crc = 0xFFFFFFFFU;
  for (size_t index = 0; index < length; ++index) {
    crc ^= input[index];
    for (uint8_t bit = 0; bit < 8U; ++bit) {
      const uint32_t mask = static_cast<uint32_t>(-(static_cast<int32_t>(crc & 1U)));
      crc = (crc >> 1U) ^ (0xEDB88320U & mask);
    }
  }
  return ~crc;
}

}  // namespace gathra::gateway
