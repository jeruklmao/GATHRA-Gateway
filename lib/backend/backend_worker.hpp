#pragma once

#include <Arduino.h>

#include "backend_contract.hpp"
#include "durable_queue.hpp"
#include "gateway_config.hpp"
#include "time_manager.hpp"

namespace gathra::gateway {

struct BackendStatus {
  char state[24]{};
  bool credentialConfigured = false;
  bool tlsEndpoint = true;
  uint64_t lastAttemptUptimeMs = 0;
  int lastHttpStatus = 0;
  uint64_t lastSuccessUptimeMs = 0;
  int64_t lastSuccessUnixMs = -1;
  char lastError[128]{};
  uint32_t currentBackoffMs = 0;
};

class BackendWorker {
 public:
  bool begin(DurableQueue& queue, TimeManager& time,
             const GatewayConfig& config, const GatewayIdentity& identity);
  void applyConfig(const GatewayConfig& config, const GatewayIdentity& identity);
  void flushNow();
  BackendStatus status() const;
  bool testConnection(int& httpStatus, char* error, size_t errorCapacity);

 private:
  struct RuntimeConfig {
    char baseUrl[build::kBackendUrlCapacity]{};
    char token[build::kBearerTokenCapacity]{};
    uint8_t batchSize = 20;
    uint32_t timeoutMs = 10'000;
    uint32_t initialBackoffMs = 2'000;
    uint32_t maximumBackoffMs = 300'000;
    GatewayIdentity identity{};
  };

  static void taskEntry(void* context);
  void run();
  bool uploadOnce(bool& completeSuccess);
  int performRequest(const RuntimeConfig& config, const char* method,
                     const char* path, const char* body, String& response,
                     char* error, size_t errorCapacity);
  bool networkAvailable(const RuntimeConfig& config) const;
  RuntimeConfig configSnapshot() const;
  void setFailure(int httpStatus, const char* message, bool advanceBackoff);
  void setSuccess(int httpStatus);

  DurableQueue* queue_ = nullptr;
  TimeManager* time_ = nullptr;
  mutable SemaphoreHandle_t mutex_ = nullptr;
  TaskHandle_t task_ = nullptr;
  RuntimeConfig config_{};
  BackendStatus status_{};
};

}  // namespace gathra::gateway
