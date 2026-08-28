#pragma once

#include <Arduino.h>

#include "backend_contract.hpp"
#include "durable_queue.hpp"
#include "gateway_config.hpp"
#include "heartbeat_contract.hpp"
#include "time_manager.hpp"

namespace gathra::gateway {

class CommandStore;
class RadioService;
class WifiManager;

struct BackendStatus {
  char state[24]{};
  bool credentialConfigured = false;
  bool tlsEndpoint = true;
  uint64_t lastAttemptUptimeMs = 0;
  int lastHttpStatus = 0;
  uint64_t lastSuccessUptimeMs = 0;
  int64_t lastSuccessUnixMs = -1;
  uint64_t lastErrorUptimeMs = 0;
  int64_t lastErrorUnixMs = -1;
  uint32_t consecutiveFailures = 0;
  uint64_t telemetryUploadSuccessCount = 0;
  uint64_t telemetryUploadFailureCount = 0;
  char lastError[128]{};
  uint32_t currentBackoffMs = 0;
};

struct HeartbeatStatus {
  uint32_t intervalSeconds = build::kDefaultHeartbeatIntervalSeconds;
  bool inProgress = false;
  uint64_t lastAttemptUptimeMs = 0;
  int64_t lastAttemptUnixMs = -1;
  uint64_t lastSuccessUptimeMs = 0;
  int64_t lastSuccessUnixMs = -1;
  int lastHttpStatus = 0;
  char lastError[128]{};
  uint64_t successCount = 0;
  uint64_t failureCount = 0;
};

class BackendWorker {
 public:
  bool begin(DurableQueue& queue, TimeManager& time,
             RadioService& radio, WifiManager& wifi, CommandStore& commands,
             const GatewayConfig& config, const GatewayIdentity& identity,
             uint32_t bootCount, const char* resetReason);
  void applyConfig(const GatewayConfig& config, const GatewayIdentity& identity);
  void flushNow();
  BackendStatus status() const;
  HeartbeatStatus heartbeatStatus() const;
  bool testConnection(int& httpStatus, char* error, size_t errorCapacity);

 private:
  struct RuntimeConfig {
    char baseUrl[build::kBackendUrlCapacity]{};
    char token[build::kBearerTokenCapacity]{};
    uint8_t batchSize = 20;
    uint32_t timeoutMs = 10'000;
    uint32_t initialBackoffMs = 2'000;
    uint32_t maximumBackoffMs = 300'000;
    uint32_t heartbeatIntervalSeconds =
        build::kDefaultHeartbeatIntervalSeconds;
    GatewayIdentity identity{};
  };

  static void taskEntry(void* context);
  void run();
  bool uploadOnce(bool& completeSuccess);
  void attemptHeartbeat();
  bool captureHeartbeatSnapshot(const RuntimeConfig& config,
                                HeartbeatSnapshot& snapshot);
  int performRequest(const RuntimeConfig& config, const char* method,
                     const char* path, const char* body, String& response,
                     char* error, size_t errorCapacity,
                     bool requireTrustedTime = true);
  bool networkAvailable(const RuntimeConfig& config) const;
  RuntimeConfig configSnapshot() const;
  void setFailure(int httpStatus, const char* message, bool advanceBackoff,
                  bool telemetryOperationFailure = false);
  void setSuccess(int httpStatus);

  DurableQueue* queue_ = nullptr;
  TimeManager* time_ = nullptr;
  RadioService* radio_ = nullptr;
  WifiManager* wifi_ = nullptr;
  CommandStore* commands_ = nullptr;
  mutable SemaphoreHandle_t mutex_ = nullptr;
  TaskHandle_t task_ = nullptr;
  RuntimeConfig config_{};
  BackendStatus status_{};
  HeartbeatStatus heartbeatStatus_{};
  HeartbeatScheduler heartbeatScheduler_{};
  uint32_t bootCount_ = 0;
  char resetReason_[32]{};
};

}  // namespace gathra::gateway
