#pragma once

#include <stddef.h>
#include <stdint.h>

#include <string>

#include "build_config.hpp"
#include "running_statistics.hpp"

namespace gathra::gateway {

inline constexpr uint8_t kHeartbeatSchemaVersion = 1U;
inline constexpr char kHeartbeatEndpoint[] = "/api/v1/iot/gateway/heartbeat";

bool heartbeatIntervalValid(uint32_t seconds);
bool heartbeatHttpSucceeded(int httpStatus);

enum class BackendConnectivityState : uint8_t {
  kUnknown,
  kHealthy,
  kDegraded,
  kOffline,
};

BackendConnectivityState classifyBackendConnectivity(
    uint64_t successfulOperations, uint32_t consecutiveFailures);
const char* backendConnectivityStateName(BackendConnectivityState state);

class HeartbeatScheduler {
 public:
  bool configure(uint32_t intervalSeconds, uint64_t nowUs);
  bool due(uint64_t nowUs) const;
  void markAttempt(uint64_t nowUs);
  uint32_t intervalSeconds() const { return intervalSeconds_; }
  uint64_t nextDueUs() const { return nextDueUs_; }

 private:
  uint32_t intervalSeconds_ = build::kDefaultHeartbeatIntervalSeconds;
  uint64_t nextDueUs_ = 0;
};

struct HeartbeatSnapshot {
  uint32_t heartbeatIntervalSeconds =
      build::kDefaultHeartbeatIntervalSeconds;

  struct Gateway {
    char gatewayId[build::kGatewayIdCapacity]{};
    char mac[18]{};
    char firmwareVersion[17]{};
    uint8_t protocolVersion = 0;
    char buildFlavor[24]{};
  } gateway;

  struct Runtime {
    uint64_t uptimeSeconds = 0;
    char resetReason[32]{};
    uint32_t bootCount = 0;
    uint32_t freeHeapBytes = 0;
    uint32_t minFreeHeapBytes = 0;
    uint32_t largestFreeHeapBlockBytes = 0;
    uint32_t sketchSizeBytes = 0;
    uint32_t freeSketchSpaceBytes = 0;
    uint32_t flashSizeBytes = 0;
  } runtime;

  struct Network {
    bool wifiConnected = false;
    char ssid[build::kWifiSsidCapacity]{};
    int32_t wifiRssiDbm = 0;
    char localIp[16]{};
    BackendConnectivityState backendConnectivityState =
        BackendConnectivityState::kUnknown;
    int64_t lastBackendSuccessUnixMs = -1;
    int64_t lastBackendErrorUnixMs = -1;
    uint32_t consecutiveBackendFailures = 0;
  } network;

  struct Time {
    bool valid = false;
    int64_t currentUnixMs = -1;
    int64_t lastNtpSyncUnixMs = -1;
  } time;

  struct Lora {
    bool paired = false;
    char pairedNodeId[build::kNodeIdCapacity]{};
    bool latestReceptionAvailable = false;
    int64_t lastRxUnixMs = -1;
    float latestRssiDbm = 0.0F;
    float latestSnrDb = 0.0F;
    int32_t latestFrequencyErrorHz = 0;
    uint64_t receivedPacketCount = 0;
    uint64_t validTelemetryCount = 0;
    uint64_t invalidPacketCount = 0;
    uint64_t crcErrorCount = 0;
    uint64_t protocolRejectedPacketCount = 0;
    uint64_t unpairedRejectedPacketCount = 0;
  } lora;

  struct Ack {
    uint64_t count = 0;
    uint64_t successCount = 0;
    uint64_t failureCount = 0;
    bool latestAvailable = false;
    uint64_t latestRxToStartUs = 0;
    uint64_t latestRxToCompleteUs = 0;
    RunningStatisticsSnapshot rxToStart{};
    RunningStatisticsSnapshot rxToComplete{};
    RunningStatisticsSnapshot txDuration{};
  } ack;

  struct Queue {
    uint32_t depth = 0;
    uint32_t capacity = 0;
    bool oldestAgeAvailable = false;
    uint64_t oldestRecordAgeSeconds = 0;
    uint64_t telemetryUploadSuccessCount = 0;
    uint64_t telemetryUploadFailureCount = 0;
  } queue;

  struct Commands {
    bool pending = false;
    uint32_t pendingCommandId = 0;
    char pendingCommandType[40]{};
    char pendingCommandState[16]{};
    bool lastAvailable = false;
    uint32_t lastCommandId = 0;
    char lastCommandResult[32]{};
    uint64_t commandsSentCount = 0;
    uint64_t commandResultsReceivedCount = 0;
  } commands;
};

bool serializeHeartbeat(const HeartbeatSnapshot& snapshot, std::string& json);

}  // namespace gathra::gateway
