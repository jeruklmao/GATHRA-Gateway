#include "heartbeat_contract.hpp"

#include <ArduinoJson.h>

#include <stdio.h>
#include <time.h>

namespace gathra::gateway {
namespace {

bool formatUtc(int64_t unixMs, char* output, size_t capacity) {
  if (unixMs < 0 || output == nullptr || capacity < 25U) return false;
  const time_t seconds = static_cast<time_t>(unixMs / 1000LL);
  tm utc{};
#ifdef _WIN32
  if (gmtime_s(&utc, &seconds) != 0) return false;
#else
  if (gmtime_r(&seconds, &utc) == nullptr) return false;
#endif
  const int64_t milliseconds = unixMs % 1000LL;
  const int written = snprintf(
      output, capacity, "%04d-%02d-%02dT%02d:%02d:%02d.%03lldZ",
      utc.tm_year + 1900, utc.tm_mon + 1, utc.tm_mday, utc.tm_hour,
      utc.tm_min, utc.tm_sec, static_cast<long long>(milliseconds));
  return written > 0 && static_cast<size_t>(written) < capacity;
}

void setTimestamp(JsonObject object, const char* field, int64_t unixMs) {
  char timestamp[32]{};
  if (formatUtc(unixMs, timestamp, sizeof(timestamp))) object[field] = timestamp;
  else object[field] = nullptr;
}

void setMilliseconds(JsonObject object, const char* field, bool available,
                     uint64_t microseconds) {
  if (available) object[field] = static_cast<double>(microseconds) / 1000.0;
  else object[field] = nullptr;
}

void setStatistics(JsonObject object, const char* minimumField,
                   const char* maximumField, const char* averageField,
                   const RunningStatisticsSnapshot& statistics) {
  if (statistics.count == 0U) {
    object[minimumField] = nullptr;
    object[maximumField] = nullptr;
    object[averageField] = nullptr;
    return;
  }
  object[minimumField] = static_cast<double>(statistics.minimum) / 1000.0;
  object[maximumField] = static_cast<double>(statistics.maximum) / 1000.0;
  object[averageField] = statistics.average / 1000.0;
}

}  // namespace

bool heartbeatIntervalValid(uint32_t seconds) {
  return seconds >= build::kMinimumHeartbeatIntervalSeconds &&
         seconds <= build::kMaximumHeartbeatIntervalSeconds;
}

bool heartbeatHttpSucceeded(int httpStatus) {
  return httpStatus >= 200 && httpStatus < 300;
}

BackendConnectivityState classifyBackendConnectivity(
    uint64_t successfulOperations, uint32_t consecutiveFailures) {
  if (successfulOperations == 0U && consecutiveFailures == 0U) {
    return BackendConnectivityState::kUnknown;
  }
  if (consecutiveFailures == 0U) return BackendConnectivityState::kHealthy;
  if (consecutiveFailures < 3U) return BackendConnectivityState::kDegraded;
  return BackendConnectivityState::kOffline;
}

const char* backendConnectivityStateName(BackendConnectivityState state) {
  switch (state) {
    case BackendConnectivityState::kUnknown: return "UNKNOWN";
    case BackendConnectivityState::kHealthy: return "HEALTHY";
    case BackendConnectivityState::kDegraded: return "DEGRADED";
    case BackendConnectivityState::kOffline: return "OFFLINE";
  }
  return "UNKNOWN";
}

bool HeartbeatScheduler::configure(uint32_t intervalSeconds, uint64_t nowUs) {
  if (!heartbeatIntervalValid(intervalSeconds)) return false;
  intervalSeconds_ = intervalSeconds;
  nextDueUs_ = nowUs + static_cast<uint64_t>(intervalSeconds_) * 1'000'000ULL;
  return true;
}

bool HeartbeatScheduler::due(uint64_t nowUs) const {
  return nextDueUs_ != 0U && nowUs >= nextDueUs_;
}

void HeartbeatScheduler::markAttempt(uint64_t nowUs) {
  nextDueUs_ = nowUs + static_cast<uint64_t>(intervalSeconds_) * 1'000'000ULL;
}

bool serializeHeartbeat(const HeartbeatSnapshot& s, std::string& json) {
  JsonDocument document;
  document["schemaVersion"] = kHeartbeatSchemaVersion;
  document["heartbeatIntervalSeconds"] = s.heartbeatIntervalSeconds;

  JsonObject gateway = document["gateway"].to<JsonObject>();
  gateway["gatewayId"] = s.gateway.gatewayId;
  gateway["mac"] = s.gateway.mac;
  gateway["firmwareVersion"] = s.gateway.firmwareVersion;
  gateway["protocolVersion"] = s.gateway.protocolVersion;
  gateway["buildFlavor"] = s.gateway.buildFlavor;

  JsonObject runtime = document["runtime"].to<JsonObject>();
  runtime["uptimeSeconds"] = s.runtime.uptimeSeconds;
  runtime["resetReason"] = s.runtime.resetReason;
  runtime["bootCount"] = s.runtime.bootCount;
  runtime["freeHeapBytes"] = s.runtime.freeHeapBytes;
  runtime["minFreeHeapBytes"] = s.runtime.minFreeHeapBytes;
  runtime["largestFreeHeapBlockBytes"] = s.runtime.largestFreeHeapBlockBytes;
  runtime["sketchSizeBytes"] = s.runtime.sketchSizeBytes;
  runtime["freeSketchSpaceBytes"] = s.runtime.freeSketchSpaceBytes;
  runtime["flashSizeBytes"] = s.runtime.flashSizeBytes;

  JsonObject network = document["network"].to<JsonObject>();
  network["wifiConnected"] = s.network.wifiConnected;
  network["ssid"] = s.network.ssid;
  if (s.network.wifiConnected) {
    network["wifiRssiDbm"] = s.network.wifiRssiDbm;
    network["localIp"] = s.network.localIp;
  } else {
    network["wifiRssiDbm"] = nullptr;
    network["localIp"] = nullptr;
  }
  network["backendConnectivityState"] =
      backendConnectivityStateName(s.network.backendConnectivityState);
  setTimestamp(network, "lastBackendSuccessAt",
               s.network.lastBackendSuccessUnixMs);
  setTimestamp(network, "lastBackendErrorAt", s.network.lastBackendErrorUnixMs);
  network["consecutiveBackendFailures"] =
      s.network.consecutiveBackendFailures;

  JsonObject time = document["time"].to<JsonObject>();
  time["timeValid"] = s.time.valid;
  if (s.time.valid) {
    setTimestamp(time, "currentUtc", s.time.currentUnixMs);
    setTimestamp(time, "lastNtpSyncAt", s.time.lastNtpSyncUnixMs);
    if (s.time.lastNtpSyncUnixMs >= 0 &&
        s.time.currentUnixMs >= s.time.lastNtpSyncUnixMs) {
      time["ntpAgeSeconds"] = static_cast<uint64_t>(
          (s.time.currentUnixMs - s.time.lastNtpSyncUnixMs) / 1000LL);
    } else {
      time["ntpAgeSeconds"] = nullptr;
    }
  } else {
    time["currentUtc"] = nullptr;
    time["lastNtpSyncAt"] = nullptr;
    time["ntpAgeSeconds"] = nullptr;
  }

  JsonObject lora = document["lora"].to<JsonObject>();
  if (s.lora.paired) lora["pairedNodeId"] = s.lora.pairedNodeId;
  else lora["pairedNodeId"] = nullptr;
  setTimestamp(lora, "lastLoRaRxAt", s.lora.lastRxUnixMs);
  if (s.lora.latestReceptionAvailable) {
    lora["latestRssiDbm"] = s.lora.latestRssiDbm;
    lora["latestSnrDb"] = s.lora.latestSnrDb;
    lora["latestFrequencyErrorHz"] = s.lora.latestFrequencyErrorHz;
  } else {
    lora["latestRssiDbm"] = nullptr;
    lora["latestSnrDb"] = nullptr;
    lora["latestFrequencyErrorHz"] = nullptr;
  }
  lora["receivedPacketCount"] = s.lora.receivedPacketCount;
  lora["validTelemetryCount"] = s.lora.validTelemetryCount;
  lora["invalidPacketCount"] = s.lora.invalidPacketCount;
  lora["crcErrorCount"] = s.lora.crcErrorCount;
  lora["protocolRejectedPacketCount"] = s.lora.protocolRejectedPacketCount;
  lora["unpairedRejectedPacketCount"] =
      s.lora.unpairedRejectedPacketCount;

  JsonObject ack = document["ack"].to<JsonObject>();
  ack["ackCount"] = s.ack.count;
  ack["ackSuccessCount"] = s.ack.successCount;
  ack["ackFailureCount"] = s.ack.failureCount;
  ack["latencySampleCount"] = s.ack.rxToStart.count;
  setMilliseconds(ack, "latestRxToAckStartMs", s.ack.latestAvailable,
                  s.ack.latestRxToStartUs);
  setMilliseconds(ack, "latestRxToAckCompleteMs", s.ack.latestAvailable,
                  s.ack.latestRxToCompleteUs);
  setMilliseconds(ack, "latestAckTxDurationMs", s.ack.latestAvailable,
                  s.ack.latestAvailable
                      ? s.ack.latestRxToCompleteUs - s.ack.latestRxToStartUs
                      : 0U);
  setStatistics(ack, "minRxToAckStartMs", "maxRxToAckStartMs",
                "avgRxToAckStartMs", s.ack.rxToStart);
  setStatistics(ack, "minRxToAckCompleteMs", "maxRxToAckCompleteMs",
                "avgRxToAckCompleteMs", s.ack.rxToComplete);
  setStatistics(ack, "minAckTxDurationMs", "maxAckTxDurationMs",
                "avgAckTxDurationMs", s.ack.txDuration);

  JsonObject queue = document["queue"].to<JsonObject>();
  queue["depth"] = s.queue.depth;
  queue["capacity"] = s.queue.capacity;
  if (s.queue.oldestAgeAvailable) {
    queue["oldestRecordAgeSeconds"] = s.queue.oldestRecordAgeSeconds;
  } else {
    queue["oldestRecordAgeSeconds"] = nullptr;
  }
  queue["telemetryUploadSuccessCount"] =
      s.queue.telemetryUploadSuccessCount;
  queue["telemetryUploadFailureCount"] =
      s.queue.telemetryUploadFailureCount;

  JsonObject commands = document["commands"].to<JsonObject>();
  if (s.commands.pending) {
    commands["pendingCommandId"] = s.commands.pendingCommandId;
    commands["pendingCommandType"] = s.commands.pendingCommandType;
    commands["pendingCommandState"] = s.commands.pendingCommandState;
  } else {
    commands["pendingCommandId"] = nullptr;
    commands["pendingCommandType"] = nullptr;
    commands["pendingCommandState"] = nullptr;
  }
  if (s.commands.lastAvailable) {
    commands["lastCommandId"] = s.commands.lastCommandId;
    commands["lastCommandResult"] = s.commands.lastCommandResult;
  } else {
    commands["lastCommandId"] = nullptr;
    commands["lastCommandResult"] = nullptr;
  }
  commands["commandsSentCount"] = s.commands.commandsSentCount;
  commands["commandResultsReceivedCount"] =
      s.commands.commandResultsReceivedCount;

  json.clear();
  serializeJson(document, json);
  return !json.empty();
}

}  // namespace gathra::gateway
