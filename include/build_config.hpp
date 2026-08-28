#pragma once

#include <stddef.h>
#include <stdint.h>

namespace gathra::gateway::build {
inline constexpr uint32_t kSerialBaud = 115200;
inline constexpr char kFallbackApPassword[] = "sman35jakarta";
inline constexpr char kMdnsHost[] = "gathra-gateway";
inline constexpr char kDefaultBackendUrl[] = "https://api.gathra.my.id";
inline constexpr size_t kNodeIdCapacity = 25;
inline constexpr size_t kGatewayIdCapacity = 49;
inline constexpr size_t kRadioPacketCapacity = 96;
inline constexpr size_t kBackendUrlCapacity = 161;
inline constexpr size_t kWifiSsidCapacity = 33;
inline constexpr size_t kWifiPasswordCapacity = 65;
inline constexpr size_t kBearerTokenCapacity = 129;
inline constexpr size_t kRecentDedupCapacity = 64;
inline constexpr size_t kLogCapacity = 96;
inline constexpr size_t kLogMessageCapacity = 176;
inline constexpr uint16_t kConfigSchemaVersion = 2;
inline constexpr uint32_t kDefaultHeartbeatIntervalSeconds = 60U;
inline constexpr uint32_t kMinimumHeartbeatIntervalSeconds = 15U;
inline constexpr uint32_t kMaximumHeartbeatIntervalSeconds = 3600U;
inline constexpr uint16_t kQueueRecordVersion = 1;
inline constexpr uint32_t kQueueMagic = 0x47545131U;  // GTQ1
inline constexpr uint32_t kQueueReservedBytes = 64U * 1024U;
inline constexpr uint32_t kEstimatedLittleFsFileOverhead = 128U;
inline constexpr uint32_t kQueueCapacityHardMax = 4096U;
inline constexpr uint8_t kBackendMaximumBatchSize = 50;
inline constexpr uint32_t kOtaValidationMagic = 0x47544F54U;  // GTOT
}  // namespace gathra::gateway::build
