#pragma once

#include <stddef.h>
#include <stdint.h>

#include "build_config.hpp"

namespace gathra::gateway {

struct RadioConfig {
  float frequencyMhz = 433.0F;
  float bandwidthKhz = 125.0F;
  uint8_t spreadingFactor = 10;
  uint8_t codingRateDenominator = 6;
  int8_t txPowerDbm = 17;
  uint8_t syncWord = 0x12;
};

struct GatewayConfig {
  uint16_t schemaVersion = build::kConfigSchemaVersion;
  char gatewayId[build::kGatewayIdCapacity]{};
  char wifiSsid[build::kWifiSsidCapacity]{};
  char wifiPassword[build::kWifiPasswordCapacity]{};
  char pairedNodeId[build::kNodeIdCapacity]{};
  char backendBaseUrl[build::kBackendUrlCapacity]{};
  char backendBearerToken[build::kBearerTokenCapacity]{};
  RadioConfig radio{};
  uint8_t backendBatchSize = 20;
  uint32_t backendHttpTimeoutMs = 10'000;
  uint32_t backendInitialBackoffMs = 2'000;
  uint32_t backendMaximumBackoffMs = 300'000;
  uint32_t wifiReconnectIntervalMs = 15'000;
  uint32_t wifiFallbackAfterMs = 60'000;
  uint32_t wifiApGraceMs = 30'000;
  uint32_t heartbeatIntervalSeconds = build::kDefaultHeartbeatIntervalSeconds;

  void setDefaults(const char* derivedGatewayId);
};

// Exact schema-1 layout used by Gateway firmware through 2.1.0. It is kept
// solely for a one-time raw-NVS migration; new writes always use GatewayConfig.
struct GatewayConfigV1 {
  uint16_t schemaVersion = 1U;
  char gatewayId[build::kGatewayIdCapacity]{};
  char wifiSsid[build::kWifiSsidCapacity]{};
  char wifiPassword[build::kWifiPasswordCapacity]{};
  char pairedNodeId[build::kNodeIdCapacity]{};
  char backendBaseUrl[build::kBackendUrlCapacity]{};
  char backendBearerToken[build::kBearerTokenCapacity]{};
  RadioConfig radio{};
  uint8_t backendBatchSize = 20;
  uint32_t backendHttpTimeoutMs = 10'000;
  uint32_t backendInitialBackoffMs = 2'000;
  uint32_t backendMaximumBackoffMs = 300'000;
  uint32_t wifiReconnectIntervalMs = 15'000;
  uint32_t wifiFallbackAfterMs = 60'000;
  uint32_t wifiApGraceMs = 30'000;
};

static_assert(offsetof(GatewayConfig, heartbeatIntervalSeconds) ==
                  sizeof(GatewayConfigV1),
              "schema-2 field must append cleanly to the schema-1 blob");

enum class ConfigValidationCode : uint8_t {
  kOk,
  kSchema,
  kGatewayId,
  kWifi,
  kNodeId,
  kBackendUrl,
  kBearerToken,
  kRadio,
  kBackendPolicy,
  kWifiPolicy,
  kHeartbeatPolicy,
};

struct ConfigValidationResult {
  ConfigValidationCode code = ConfigValidationCode::kOk;
  const char* message = "ok";
  explicit operator bool() const { return code == ConfigValidationCode::kOk; }
};

bool gatewayIdValid(const char* gatewayId);
bool backendUrlValid(const char* url);
bool radioConfigEqual(const RadioConfig& lhs, const RadioConfig& rhs);
ConfigValidationResult validateConfig(const GatewayConfig& config);
bool migrateConfigV1(const GatewayConfigV1& legacy, GatewayConfig& migrated);

}  // namespace gathra::gateway
