#pragma once

#include <stddef.h>
#include <stdint.h>

#include <vector>

#include "build_config.hpp"

namespace gathra::gateway {

struct ReceptionMetadata {
  int64_t gatewayReceivedUnixMs = -1;
  bool gatewayTimeTrusted = false;
  uint64_t gatewayUptimeMs = 0;
  uint32_t gatewayBootSessionId = 0;
  float rssiDbm = 0.0F;
  float snrDb = 0.0F;
  int32_t frequencyErrorHz = 0;
  uint16_t packetLength = 0;
};

struct QueueRecord {
  uint64_t recordId = 0;
  ReceptionMetadata reception{};
  uint8_t payloadLength = 0;
  uint8_t rawPayload[build::kRadioPacketCapacity]{};
};

class QueueRecordCodec {
 public:
  static constexpr size_t kFixedBytesWithoutPayload = 56U;
  static constexpr size_t kMaximumEncodedBytes =
      kFixedBytesWithoutPayload + build::kRadioPacketCapacity;

  static bool encode(const QueueRecord& record, std::vector<uint8_t>& output);
  static bool decode(const uint8_t* input, size_t length, QueueRecord& record);
  static uint32_t crc32(const uint8_t* input, size_t length);
};

}  // namespace gathra::gateway
