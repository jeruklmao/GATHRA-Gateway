#ifdef ARDUINO

#include "backend_worker.hpp"

#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>
#include <esp_heap_caps.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <string.h>

#include "logger.hpp"
#include "command_store.hpp"
#include "firmware_version.hpp"
#include "protocol.hpp"
#include "radio_service.hpp"
#include "tls_trust.hpp"
#include "wifi_manager.hpp"

namespace gathra::gateway {
namespace {

class Lock {
 public:
  explicit Lock(SemaphoreHandle_t mutex) : mutex_(mutex) {
    locked_ = mutex_ != nullptr && xSemaphoreTake(mutex_, portMAX_DELAY) == pdTRUE;
  }
  ~Lock() { if (locked_) xSemaphoreGive(mutex_); }
  explicit operator bool() const { return locked_; }
 private:
  SemaphoreHandle_t mutex_;
  bool locked_ = false;
};

bool startsWith(const char* value, const char* prefix) {
  return value != nullptr && prefix != nullptr &&
         strncmp(value, prefix, strlen(prefix)) == 0;
}

uint64_t uptimeMs() {
  return static_cast<uint64_t>(esp_timer_get_time()) / 1000ULL;
}

template <size_t N>
void copyText(char (&destination)[N], const char* source) {
  strncpy(destination, source == nullptr ? "" : source, N - 1U);
  destination[N - 1U] = '\0';
}

}  // namespace

bool BackendWorker::begin(DurableQueue& queue, TimeManager& time,
                          RadioService& radio, WifiManager& wifi,
                          CommandStore& commands, const GatewayConfig& config,
                          const GatewayIdentity& identity, uint32_t bootCount,
                          const char* resetReason) {
  queue_ = &queue;
  time_ = &time;
  radio_ = &radio;
  wifi_ = &wifi;
  commands_ = &commands;
  bootCount_ = bootCount;
  copyText(resetReason_, resetReason);
  mutex_ = xSemaphoreCreateMutex();
  if (mutex_ == nullptr) return false;
  applyConfig(config, identity);
  strncpy(status_.state, "IDLE", sizeof(status_.state) - 1U);
  return xTaskCreate(taskEntry, "gth-backend", 10240, this, 1, &task_) == pdPASS;
}

void BackendWorker::applyConfig(const GatewayConfig& config,
                                const GatewayIdentity& identity) {
  Lock lock(mutex_);
  if (!lock) return;
  strncpy(config_.baseUrl, config.backendBaseUrl, sizeof(config_.baseUrl) - 1U);
  config_.baseUrl[sizeof(config_.baseUrl) - 1U] = '\0';
  strncpy(config_.token, config.backendBearerToken, sizeof(config_.token) - 1U);
  config_.token[sizeof(config_.token) - 1U] = '\0';
  config_.batchSize = config.backendBatchSize;
  config_.timeoutMs = config.backendHttpTimeoutMs;
  config_.initialBackoffMs = config.backendInitialBackoffMs;
  config_.maximumBackoffMs = config.backendMaximumBackoffMs;
  config_.heartbeatIntervalSeconds = config.heartbeatIntervalSeconds;
  config_.identity = identity;
  status_.credentialConfigured = config_.token[0] != '\0';
  status_.tlsEndpoint = startsWith(config_.baseUrl, "https://");
  status_.currentBackoffMs = 0;
  (void)heartbeatScheduler_.configure(config_.heartbeatIntervalSeconds,
                                      static_cast<uint64_t>(esp_timer_get_time()));
  heartbeatStatus_.intervalSeconds = config_.heartbeatIntervalSeconds;
  if (task_ != nullptr) xTaskNotifyGive(task_);
}

void BackendWorker::flushNow() {
  if (task_ != nullptr) xTaskNotifyGive(task_);
}

BackendStatus BackendWorker::status() const {
  Lock lock(mutex_);
  return lock ? status_ : BackendStatus{};
}

HeartbeatStatus BackendWorker::heartbeatStatus() const {
  Lock lock(mutex_);
  return lock ? heartbeatStatus_ : HeartbeatStatus{};
}

bool BackendWorker::testConnection(int& httpStatus, char* error,
                                   size_t errorCapacity) {
  const RuntimeConfig config = configSnapshot();
  String response;
  httpStatus = performRequest(config, "GET", "/api/v1/iot/gateway/ping", nullptr,
                              response, error, errorCapacity);
  return httpStatus == HTTP_CODE_OK;
}

void BackendWorker::taskEntry(void* context) {
  static_cast<BackendWorker*>(context)->run();
}

void BackendWorker::run() {
  while (true) {
    if (queue_ == nullptr || queue_->size() == 0U) {
      {
        Lock lock(mutex_);
        if (lock) strncpy(status_.state, "IDLE", sizeof(status_.state) - 1U);
      }
      bool heartbeatDue = false;
      {
        Lock lock(mutex_);
        heartbeatDue = lock && heartbeatScheduler_.due(
                                    static_cast<uint64_t>(esp_timer_get_time()));
      }
      if (heartbeatDue && queue_ != nullptr && queue_->size() == 0U) {
        attemptHeartbeat();
        continue;
      }
      (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));
      continue;
    }
    const RuntimeConfig config = configSnapshot();
    if (!networkAvailable(config)) {
      setFailure(0, "Wi-Fi unavailable; queue retained", false);
      (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(5000));
      continue;
    }
    if (config.token[0] == '\0') {
      setFailure(0, "backend credential is not configured; queue retained", false);
      (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(5000));
      continue;
    }
    if (startsWith(config.baseUrl, "https://") && !time_->now().trusted) {
      setFailure(0, "HTTPS waiting for trusted NTP time; queue retained", false);
      (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(5000));
      continue;
    }
    bool completeSuccess = false;
    const bool requestCompleted = uploadOnce(completeSuccess);
    if (requestCompleted && completeSuccess) {
      vTaskDelay(pdMS_TO_TICKS(50));
      continue;
    }
    const BackendStatus current = status();
    const uint32_t jitter = current.currentBackoffMs == 0U
                                ? 0U
                                : esp_random() % (current.currentBackoffMs / 5U + 1U);
    (void)ulTaskNotifyTake(pdTRUE,
                          pdMS_TO_TICKS(current.currentBackoffMs + jitter));
  }
}

bool BackendWorker::uploadOnce(bool& completeSuccess) {
  completeSuccess = false;
  const RuntimeConfig config = configSnapshot();
  std::vector<QueueRecord> records;
  if (!queue_->peek(config.batchSize, records) || records.empty()) {
    setFailure(0, "queue batch read failed", true);
    queue_->recordFilesystemError();
    return false;
  }
  std::string payload;
  if (!serializeBackendBatch(config.identity, records, payload)) {
    setFailure(0, "backend batch serialization failed", true);
    return false;
  }
  {
    Lock lock(mutex_);
    if (lock) {
      strncpy(status_.state, "UPLOADING", sizeof(status_.state) - 1U);
      status_.lastAttemptUptimeMs = uptimeMs();
    }
  }
  String response;
  char error[128]{};
  const int httpStatus = performRequest(config, "POST",
                                        "/api/v1/iot/telemetry/batch",
                                        payload.c_str(), response,
                                        error, sizeof(error));
  if (!backendHttpResponseMayDequeue(httpStatus)) {
    const char* message = error[0] != '\0'
                              ? error
                              : (httpStatus == HTTP_CODE_UNAUTHORIZED ||
                                         httpStatus == HTTP_CODE_FORBIDDEN
                                     ? "backend authentication rejected; queue retained"
                                     : "backend request failed; queue retained");
    setFailure(httpStatus, message, true, true);
    queue_->recordUploadRetry();
    GTH_LOGW("BACKEND", "upload failed status=%d queue retained", httpStatus);
    return false;
  }
  std::vector<BackendRecordResult> results;
  if (!parseBackendBatchResponse(response.c_str(), records.size(), results)) {
    setFailure(httpStatus, "incompatible/malformed batch response; queue retained",
               true, true);
    queue_->recordUploadRetry();
    return false;
  }
  std::vector<QueueTerminalAction> actions;
  actions.reserve(results.size());
  for (const BackendRecordResult& result : results) {
    protocol::TelemetryPacket telemetry{};
    const QueueRecord& record = records[result.index];
    if (protocol::decodeTelemetry(record.rawPayload, record.payloadLength, telemetry) !=
        protocol::DecodeStatus::kOk) {
      setFailure(httpStatus, "queued payload unexpectedly failed local decode", true);
      return false;
    }
    QueueTerminalAction action{};
    action.recordId = record.recordId;
    action.key = makeTelemetryKey(telemetry.nodeId, telemetry.persistentSessionId,
                                  telemetry.sequence);
    action.permanentlyRejected =
        result.status == BackendRecordStatus::kRejectedInvalid;
    actions.push_back(action);
    if (action.permanentlyRejected) {
      GTH_LOGE("BACKEND", "permanent rejection node=%s boot=%lu seq=%lu reason=%s",
               telemetry.nodeId,
               static_cast<unsigned long>(telemetry.persistentSessionId),
               static_cast<unsigned long>(telemetry.sequence),
               result.reason[0] == '\0' ? "unspecified" : result.reason);
    }
  }
  if (!actions.empty() && !queue_->finish(actions)) {
    setFailure(httpStatus, "terminal queue dequeue failed", true);
    return false;
  }
  if (!backendBatchResponseComplete(records.size(), results)) {
    setFailure(httpStatus, "partial batch response; missing records retained", true,
               true);
    queue_->recordUploadRetry();
    return true;
  }
  setSuccess(httpStatus);
  completeSuccess = true;
  GTH_LOGI("BACKEND", "batch terminal records=%u remaining=%u",
           static_cast<unsigned>(actions.size()),
           static_cast<unsigned>(queue_->size()));
  return true;
}

void BackendWorker::attemptHeartbeat() {
  if (queue_ != nullptr && queue_->size() != 0U) return;
  const uint64_t attemptUptimeMs = uptimeMs();
  const TrustedTimeSnapshot attemptTime = time_->now();
  {
    Lock lock(mutex_);
    if (!lock) return;
    heartbeatScheduler_.markAttempt(
        static_cast<uint64_t>(esp_timer_get_time()));
    heartbeatStatus_.inProgress = true;
    heartbeatStatus_.lastAttemptUptimeMs = attemptUptimeMs;
    heartbeatStatus_.lastAttemptUnixMs =
        attemptTime.trusted ? attemptTime.unixMs : -1;
  }

  const RuntimeConfig config = configSnapshot();
  HeartbeatSnapshot snapshot{};
  std::string payload;
  char error[128]{};
  int httpStatus = 0;
  bool deferredForTelemetry = false;
  if (queue_ != nullptr && queue_->size() != 0U) {
    deferredForTelemetry = true;
  } else if (!networkAvailable(config)) {
    snprintf(error, sizeof(error), "Wi-Fi unavailable");
  } else if (config.token[0] == '\0') {
    snprintf(error, sizeof(error), "backend credential is not configured");
  } else if (!captureHeartbeatSnapshot(config, snapshot) ||
             !serializeHeartbeat(snapshot, payload)) {
    snprintf(error, sizeof(error), "heartbeat serialization failed");
  } else if (queue_ != nullptr && queue_->size() != 0U) {
    deferredForTelemetry = true;
  } else {
    RuntimeConfig requestConfig = config;
    if (requestConfig.timeoutMs > 5'000U) requestConfig.timeoutMs = 5'000U;
    String response;
    httpStatus = performRequest(requestConfig, "POST", kHeartbeatEndpoint,
                                payload.c_str(), response, error,
                                sizeof(error), false);
  }

  const bool success = heartbeatHttpSucceeded(httpStatus);
  const TrustedTimeSnapshot completedAt = time_->now();
  {
    Lock lock(mutex_);
    if (!lock) return;
    heartbeatStatus_.inProgress = false;
    heartbeatStatus_.lastHttpStatus = httpStatus;
    if (deferredForTelemetry) {
      copyText(heartbeatStatus_.lastError, "deferred for durable telemetry");
    } else if (success) {
      ++heartbeatStatus_.successCount;
      heartbeatStatus_.lastSuccessUptimeMs = uptimeMs();
      heartbeatStatus_.lastSuccessUnixMs =
          completedAt.trusted ? completedAt.unixMs : -1;
      heartbeatStatus_.lastError[0] = '\0';
    } else {
      ++heartbeatStatus_.failureCount;
      if (error[0] != '\0') {
        copyText(heartbeatStatus_.lastError, error);
      } else if (httpStatus > 0) {
        snprintf(heartbeatStatus_.lastError,
                 sizeof(heartbeatStatus_.lastError), "HTTP %d", httpStatus);
      } else {
        copyText(heartbeatStatus_.lastError, "heartbeat request failed");
      }
    }
  }
  if (deferredForTelemetry) {
    GTH_LOGI("HEARTBEAT", "deferred for durable telemetry");
  } else if (success) {
    GTH_LOGI("HEARTBEAT", "delivered status=%d", httpStatus);
  } else {
    GTH_LOGW("HEARTBEAT", "best-effort attempt failed status=%d", httpStatus);
  }
}

bool BackendWorker::captureHeartbeatSnapshot(const RuntimeConfig& config,
                                             HeartbeatSnapshot& snapshot) {
  if (queue_ == nullptr || time_ == nullptr || radio_ == nullptr ||
      wifi_ == nullptr || commands_ == nullptr) {
    return false;
  }
  copyText(snapshot.gateway.gatewayId, config.identity.gatewayId);
  copyText(snapshot.gateway.mac, config.identity.hardwareMac);
  copyText(snapshot.gateway.firmwareVersion, firmware::kVersion);
  snapshot.gateway.protocolVersion = protocol::kVersion;
  copyText(snapshot.gateway.buildFlavor, firmware::kBuildFlavor);
  snapshot.heartbeatIntervalSeconds = config.heartbeatIntervalSeconds;

  snapshot.runtime.uptimeSeconds = uptimeMs() / 1000ULL;
  copyText(snapshot.runtime.resetReason, resetReason_);
  snapshot.runtime.bootCount = bootCount_;
  snapshot.runtime.freeHeapBytes = ESP.getFreeHeap();
  snapshot.runtime.minFreeHeapBytes = ESP.getMinFreeHeap();
  snapshot.runtime.largestFreeHeapBlockBytes =
      heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
  snapshot.runtime.sketchSizeBytes = ESP.getSketchSize();
  snapshot.runtime.freeSketchSpaceBytes = ESP.getFreeSketchSpace();
  snapshot.runtime.flashSizeBytes = ESP.getFlashChipSize();

  const WifiStatus wifi = wifi_->status();
  snapshot.network.wifiConnected = wifi.connected;
  copyText(snapshot.network.ssid, wifi.ssid);
  snapshot.network.wifiRssiDbm = wifi.rssiDbm;
  copyText(snapshot.network.localIp, wifi.localIp);
  const BackendStatus backend = status();
  snapshot.network.backendConnectivityState = classifyBackendConnectivity(
      backend.telemetryUploadSuccessCount, backend.consecutiveFailures);
  snapshot.network.lastBackendSuccessUnixMs = backend.lastSuccessUnixMs;
  snapshot.network.lastBackendErrorUnixMs = backend.lastErrorUnixMs;
  snapshot.network.consecutiveBackendFailures = backend.consecutiveFailures;

  const TimeStatus time = time_->status();
  snapshot.time.valid = time.trusted;
  snapshot.time.currentUnixMs = time.trusted ? time.currentUnixMs : -1;
  snapshot.time.lastNtpSyncUnixMs = time.trusted ? time.lastSyncUnixMs : -1;

  const RadioDiagnostics radio = radio_->diagnostics();
  snapshot.lora.paired = radio_->paired();
  radio_->pairedNodeId(snapshot.lora.pairedNodeId,
                       sizeof(snapshot.lora.pairedNodeId));
  snapshot.lora.latestReceptionAvailable = radio.lastRxUptimeUs != 0U;
  snapshot.lora.lastRxUnixMs = radio.lastRxUnixMs;
  snapshot.lora.latestRssiDbm = radio.lastRssiDbm;
  snapshot.lora.latestSnrDb = radio.lastSnrDb;
  snapshot.lora.latestFrequencyErrorHz = radio.lastFrequencyErrorHz;
  snapshot.lora.receivedPacketCount = radio.receivedPackets;
  snapshot.lora.validTelemetryCount = radio.processing.validTelemetryPackets;
  snapshot.lora.crcErrorCount = radio.crcErrors;
  snapshot.lora.protocolRejectedPacketCount = radio.processing.decodeErrors;
  snapshot.lora.unpairedRejectedPacketCount =
      radio.processing.unknownNodePackets;
  snapshot.lora.invalidPacketCount =
      radio.crcErrors + radio.invalidLengthPackets + radio.readErrors +
      radio.processing.decodeErrors;

  snapshot.ack.successCount = radio.processing.ackSent;
  snapshot.ack.failureCount = radio.processing.ackFailures;
  snapshot.ack.count = snapshot.ack.successCount + snapshot.ack.failureCount;
  snapshot.ack.latestAvailable =
      snapshot.ack.count > 0U &&
      radio.processing.lastRxToAckCompleteUs > 0U &&
      radio.processing.lastRxToAckCompleteUs >=
          radio.processing.lastRxToAckStartUs;
  snapshot.ack.latestRxToStartUs = radio.processing.lastRxToAckStartUs;
  snapshot.ack.latestRxToCompleteUs = radio.processing.lastRxToAckCompleteUs;
  snapshot.ack.rxToStart =
      radio.processing.successfulAckLatency.rxToStart.snapshot();
  snapshot.ack.rxToComplete =
      radio.processing.successfulAckLatency.rxToComplete.snapshot();
  snapshot.ack.txDuration =
      radio.processing.successfulAckLatency.txDuration.snapshot();

  snapshot.queue.depth = static_cast<uint32_t>(queue_->size());
  snapshot.queue.capacity = static_cast<uint32_t>(queue_->capacity());
  snapshot.queue.telemetryUploadSuccessCount =
      backend.telemetryUploadSuccessCount;
  snapshot.queue.telemetryUploadFailureCount =
      backend.telemetryUploadFailureCount;
  if (snapshot.queue.depth > 0U) {
    std::vector<QueueRecord> oldest;
    if (queue_->peek(1U, oldest) && !oldest.empty()) {
      int64_t ageMs = -1;
      if (time.trusted && oldest[0].reception.gatewayTimeTrusted) {
        ageMs = time.currentUnixMs - oldest[0].reception.gatewayReceivedUnixMs;
      } else if (oldest[0].reception.gatewayBootSessionId ==
                 config.identity.bootSessionId) {
        ageMs = static_cast<int64_t>(uptimeMs()) -
                static_cast<int64_t>(oldest[0].reception.gatewayUptimeMs);
      }
      if (ageMs >= 0) {
        snapshot.queue.oldestAgeAvailable = true;
        snapshot.queue.oldestRecordAgeSeconds =
            static_cast<uint64_t>(ageMs) / 1000ULL;
      }
    }
  }

  const GatewayCommand command = commands_->current();
  snapshot.commands.pending = command.state == CommandState::kPending ||
                              command.state == CommandState::kSent;
  snapshot.commands.pendingCommandId = command.commandId;
  copyText(snapshot.commands.pendingCommandType,
           protocol::commandTypeName(command.type));
  copyText(snapshot.commands.pendingCommandState,
           commandStateName(command.state));
  snapshot.commands.lastAvailable = command.commandId != 0U;
  snapshot.commands.lastCommandId = command.commandId;
  copyText(snapshot.commands.lastCommandResult,
           protocol::commandResultName(command.result));
  snapshot.commands.commandsSentCount = radio.commandsSentCount;
  snapshot.commands.commandResultsReceivedCount =
      radio.processing.validCommandResultPackets;
  return true;
}

int BackendWorker::performRequest(const RuntimeConfig& config,
                                  const char* method, const char* path,
                                  const char* body, String& response,
                                  char* error, size_t errorCapacity,
                                  bool requireTrustedTime) {
  if (error != nullptr && errorCapacity > 0U) error[0] = '\0';
  if (!networkAvailable(config)) {
    if (error != nullptr) snprintf(error, errorCapacity, "Wi-Fi disconnected");
    return 0;
  }
  const bool tls = startsWith(config.baseUrl, "https://");
  if (tls && requireTrustedTime &&
      (time_ == nullptr || !time_->now().trusted)) {
    if (error != nullptr) snprintf(error, errorCapacity, "TLS requires trusted time");
    return 0;
  }
  const String url = String(config.baseUrl) + path;
  HTTPClient http;
  http.setConnectTimeout(config.timeoutMs);
  http.setTimeout(config.timeoutMs);
  WiFiClient plain;
  WiFiClientSecure secure;
  bool begun = false;
  if (tls) {
    secure.setCACert(kGtsRootR4Pem);
    begun = http.begin(secure, url);
  } else {
    begun = http.begin(plain, url);
  }
  if (!begun) {
    if (error != nullptr) snprintf(error, errorCapacity, "HTTP client initialization failed");
    return 0;
  }
  http.addHeader("Accept", "application/json");
  http.addHeader("Authorization", String("Bearer ") + config.token);
  int status = 0;
  if (strcmp(method, "POST") == 0) {
    http.addHeader("Content-Type", "application/json");
    status = http.POST(String(body));
  } else {
    status = http.GET();
  }
  if (status > 0) response = http.getString();
  else if (error != nullptr) {
    snprintf(error, errorCapacity, "transport error %s",
             HTTPClient::errorToString(status).c_str());
  }
  http.end();
  return status;
}

bool BackendWorker::networkAvailable(const RuntimeConfig& config) const {
  if (WiFi.status() == WL_CONNECTED) return true;
  const wifi_mode_t mode = WiFi.getMode();
  const bool apActive = mode == WIFI_AP || mode == WIFI_AP_STA;
  // Production HTTPS remains STA-only. Explicit local HTTP can reach a HIL
  // Backend running on a client associated with the fallback AP.
  return startsWith(config.baseUrl, "http://") && apActive &&
         WiFi.softAPgetStationNum() > 0U;
}

BackendWorker::RuntimeConfig BackendWorker::configSnapshot() const {
  Lock lock(mutex_);
  return lock ? config_ : RuntimeConfig{};
}

void BackendWorker::setFailure(int httpStatus, const char* message,
                               bool advanceBackoff,
                               bool telemetryOperationFailure) {
  Lock lock(mutex_);
  if (!lock) return;
  strncpy(status_.state, "BACKOFF", sizeof(status_.state) - 1U);
  status_.lastHttpStatus = httpStatus;
  strncpy(status_.lastError, message == nullptr ? "unknown error" : message,
          sizeof(status_.lastError) - 1U);
  status_.lastError[sizeof(status_.lastError) - 1U] = '\0';
  if (telemetryOperationFailure) {
    ++status_.telemetryUploadFailureCount;
    if (status_.consecutiveFailures != UINT32_MAX) {
      ++status_.consecutiveFailures;
    }
    status_.lastErrorUptimeMs = uptimeMs();
    const TrustedTimeSnapshot now = time_->now();
    status_.lastErrorUnixMs = now.trusted ? now.unixMs : -1;
  }
  if (advanceBackoff) {
    if (status_.currentBackoffMs == 0U) {
      status_.currentBackoffMs = config_.initialBackoffMs;
    } else {
      const uint64_t doubled =
          static_cast<uint64_t>(status_.currentBackoffMs) * 2ULL;
      status_.currentBackoffMs = static_cast<uint32_t>(
          doubled > config_.maximumBackoffMs ? config_.maximumBackoffMs
                                             : doubled);
    }
  }
}

void BackendWorker::setSuccess(int httpStatus) {
  Lock lock(mutex_);
  if (!lock) return;
  strncpy(status_.state, "CONNECTED", sizeof(status_.state) - 1U);
  status_.lastHttpStatus = httpStatus;
  status_.lastError[0] = '\0';
  status_.currentBackoffMs = 0;
  status_.lastSuccessUptimeMs = uptimeMs();
  const TrustedTimeSnapshot now = time_->now();
  status_.lastSuccessUnixMs = now.trusted ? now.unixMs : -1;
  status_.consecutiveFailures = 0U;
  ++status_.telemetryUploadSuccessCount;
}

}  // namespace gathra::gateway

#endif  // ARDUINO
