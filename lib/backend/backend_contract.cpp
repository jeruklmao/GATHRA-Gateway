#include "backend_contract.hpp"

#include <ArduinoJson.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "base64.hpp"

namespace gathra::gateway {
namespace {

bool formatUtc(int64_t unixMs, char* output, size_t capacity) {
  if (unixMs < 0 || output == nullptr || capacity < 25U) return false;
  const time_t seconds = static_cast<time_t>(unixMs / 1000LL);
  tm utc{};
#if defined(_WIN32)
  if (gmtime_s(&utc, &seconds) != 0) return false;
#else
  if (gmtime_r(&seconds, &utc) == nullptr) return false;
#endif
  const int written = snprintf(output, capacity,
                               "%04d-%02d-%02dT%02d:%02d:%02d.%03lldZ",
                               utc.tm_year + 1900, utc.tm_mon + 1, utc.tm_mday,
                               utc.tm_hour, utc.tm_min, utc.tm_sec,
                               static_cast<long long>(unixMs % 1000LL));
  return written > 0 && static_cast<size_t>(written) < capacity;
}

}  // namespace

bool serializeBackendBatch(const GatewayIdentity& identity,
                           const std::vector<QueueRecord>& records,
                           std::string& json) {
  json.clear();
  if (records.empty() || records.size() > build::kBackendMaximumBatchSize) return false;
  JsonDocument document;
  document["schemaVersion"] = 1;
  JsonObject gateway = document["gateway"].to<JsonObject>();
  gateway["gatewayId"] = identity.gatewayId;
  gateway["hardwareMac"] = identity.hardwareMac;
  gateway["firmwareVersion"] = identity.firmwareVersion;
  gateway["bootSessionId"] = identity.bootSessionId;
  JsonArray readings = document["readings"].to<JsonArray>();
  for (const QueueRecord& record : records) {
    if (record.payloadLength == 0U || record.payloadLength > build::kRadioPacketCapacity) return false;
    JsonObject reading = readings.add<JsonObject>();
    if (record.reception.gatewayTimeTrusted) {
      char timestamp[32]{};
      if (!formatUtc(record.reception.gatewayReceivedUnixMs, timestamp, sizeof(timestamp))) return false;
      reading["gatewayReceivedAt"] = timestamp;
    } else {
      reading["gatewayReceivedAt"] = nullptr;
    }
    reading["gatewayTimeTrusted"] = record.reception.gatewayTimeTrusted;
    reading["gatewayUptimeMs"] = record.reception.gatewayUptimeMs;
    // This is intentionally per-reading: a recovered queue may contain records
    // captured by an earlier Gateway boot than the process performing upload.
    reading["gatewayBootSessionId"] =
        record.reception.gatewayBootSessionId;
    reading["rssiDbm"] = record.reception.rssiDbm;
    reading["snrDb"] = record.reception.snrDb;
    reading["frequencyErrorHz"] = record.reception.frequencyErrorHz;
    reading["packetLength"] = record.reception.packetLength;
    const std::string encoded = base64Encode(record.rawPayload, record.payloadLength);
    reading["rawPayloadBase64"] = encoded;
  }
  serializeJson(document, json);
  return !json.empty();
}

bool parseBackendBatchResponse(const char* json, size_t expectedRecordCount,
                               std::vector<BackendRecordResult>& results) {
  results.clear();
  if (json == nullptr || expectedRecordCount == 0U) return false;
  JsonDocument document;
  if (deserializeJson(document, json) != DeserializationError::Ok) return false;
  JsonArrayConst array = document["results"].as<JsonArrayConst>();
  if (array.isNull() || array.size() > expectedRecordCount) return false;
  bool seen[build::kBackendMaximumBatchSize]{};
  for (JsonObjectConst item : array) {
    if (!item["index"].is<size_t>() || !item["status"].is<const char*>()) return false;
    const size_t index = item["index"].as<size_t>();
    if (index >= expectedRecordCount || seen[index]) return false;
    seen[index] = true;
    const char* value = item["status"].as<const char*>();
    BackendRecordStatus status{};
    if (strcmp(value, "INSERTED") == 0) status = BackendRecordStatus::kInserted;
    else if (strcmp(value, "DUPLICATE") == 0) status = BackendRecordStatus::kDuplicate;
    else if (strcmp(value, "REJECTED_INVALID") == 0) status = BackendRecordStatus::kRejectedInvalid;
    else return false;
    BackendRecordResult result{};
    result.index = index;
    result.status = status;
    const char* reason = item["reason"].is<const char*>()
                             ? item["reason"].as<const char*>()
                             : "";
    strncpy(result.reason, reason, sizeof(result.reason) - 1U);
    results.push_back(result);
  }
  return true;
}

bool backendHttpResponseMayDequeue(int httpStatus) {
  // Authentication failures, validation-envelope failures, redirects, and
  // transport/server failures are all non-terminal for queued readings.
  return httpStatus == 200;
}

bool backendBatchResponseComplete(
    size_t expectedRecordCount,
    const std::vector<BackendRecordResult>& results) {
  return expectedRecordCount > 0U && results.size() == expectedRecordCount;
}

const char* backendRecordStatusName(BackendRecordStatus status) {
  switch (status) {
    case BackendRecordStatus::kInserted: return "INSERTED";
    case BackendRecordStatus::kDuplicate: return "DUPLICATE";
    case BackendRecordStatus::kRejectedInvalid: return "REJECTED_INVALID";
  }
  return "UNKNOWN";
}

}  // namespace gathra::gateway
