#ifdef ARDUINO

#include "backend_worker.hpp"

#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>
#include <esp_system.h>
#include <string.h>

#include "logger.hpp"
#include "protocol.hpp"
#include "tls_trust.hpp"

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

}  // namespace

bool BackendWorker::begin(DurableQueue& queue, TimeManager& time,
                          const GatewayConfig& config,
                          const GatewayIdentity& identity) {
  queue_ = &queue;
  time_ = &time;
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
  config_.identity = identity;
  status_.credentialConfigured = config_.token[0] != '\0';
  status_.tlsEndpoint = startsWith(config_.baseUrl, "https://");
  status_.currentBackoffMs = 0;
  if (task_ != nullptr) xTaskNotifyGive(task_);
}

void BackendWorker::flushNow() {
  if (task_ != nullptr) xTaskNotifyGive(task_);
}

BackendStatus BackendWorker::status() const {
  Lock lock(mutex_);
  return lock ? status_ : BackendStatus{};
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
      (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(5000));
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
      status_.lastAttemptUptimeMs = millis();
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
    setFailure(httpStatus, message, true);
    queue_->recordUploadRetry();
    GTH_LOGW("BACKEND", "upload failed status=%d queue retained", httpStatus);
    return false;
  }
  std::vector<BackendRecordResult> results;
  if (!parseBackendBatchResponse(response.c_str(), records.size(), results)) {
    setFailure(httpStatus, "incompatible/malformed batch response; queue retained", true);
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
    action.key = makeTelemetryKey(telemetry.nodeId, telemetry.bootSessionId,
                                  telemetry.sequence);
    action.permanentlyRejected =
        result.status == BackendRecordStatus::kRejectedInvalid;
    actions.push_back(action);
    if (action.permanentlyRejected) {
      GTH_LOGE("BACKEND", "permanent rejection node=%s boot=%lu seq=%lu reason=%s",
               telemetry.nodeId,
               static_cast<unsigned long>(telemetry.bootSessionId),
               static_cast<unsigned long>(telemetry.sequence),
               result.reason[0] == '\0' ? "unspecified" : result.reason);
    }
  }
  if (!actions.empty() && !queue_->finish(actions)) {
    setFailure(httpStatus, "terminal queue dequeue failed", true);
    return false;
  }
  if (!backendBatchResponseComplete(records.size(), results)) {
    setFailure(httpStatus, "partial batch response; missing records retained", true);
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

int BackendWorker::performRequest(const RuntimeConfig& config,
                                  const char* method, const char* path,
                                  const char* body, String& response,
                                  char* error, size_t errorCapacity) {
  if (error != nullptr && errorCapacity > 0U) error[0] = '\0';
  if (!networkAvailable(config)) {
    if (error != nullptr) snprintf(error, errorCapacity, "Wi-Fi disconnected");
    return 0;
  }
  const bool tls = startsWith(config.baseUrl, "https://");
  if (tls && (time_ == nullptr || !time_->now().trusted)) {
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
                               bool advanceBackoff) {
  Lock lock(mutex_);
  if (!lock) return;
  strncpy(status_.state, "BACKOFF", sizeof(status_.state) - 1U);
  status_.lastHttpStatus = httpStatus;
  strncpy(status_.lastError, message == nullptr ? "unknown error" : message,
          sizeof(status_.lastError) - 1U);
  status_.lastError[sizeof(status_.lastError) - 1U] = '\0';
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
  status_.lastSuccessUptimeMs = millis();
  const TrustedTimeSnapshot now = time_->now();
  status_.lastSuccessUnixMs = now.trusted ? now.unixMs : -1;
}

}  // namespace gathra::gateway

#endif  // ARDUINO
