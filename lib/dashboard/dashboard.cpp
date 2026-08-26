#ifdef ARDUINO

#include "dashboard.hpp"

#include <ArduinoJson.h>
#include <Update.h>
#include <esp_timer.h>
#include <math.h>
#include <memory>
#include <new>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "dashboard_html.hpp"
#include "firmware_version.hpp"
#include "logger.hpp"
#include "protocol.hpp"

namespace gathra::gateway {
namespace {

bool parseUnsigned(const String& value, uint32_t& output) {
  if (value.isEmpty()) return false;
  char* end = nullptr;
  errno = 0;
  const unsigned long parsed = strtoul(value.c_str(), &end, 10);
  if (errno != 0 || end == value.c_str() || *end != '\0' || parsed > UINT32_MAX) return false;
  output = static_cast<uint32_t>(parsed);
  return true;
}

bool parseSigned(const String& value, int32_t& output) {
  if (value.isEmpty()) return false;
  char* end = nullptr;
  errno = 0;
  const long parsed = strtol(value.c_str(), &end, 10);
  if (errno != 0 || end == value.c_str() || *end != '\0' ||
      parsed < INT32_MIN || parsed > INT32_MAX) return false;
  output = static_cast<int32_t>(parsed);
  return true;
}

bool parseFloat(const String& value, float& output) {
  if (value.isEmpty()) return false;
  char* end = nullptr;
  errno = 0;
  const float parsed = strtof(value.c_str(), &end);
  if (errno != 0 || end == value.c_str() || *end != '\0' || !isfinite(parsed)) return false;
  output = parsed;
  return true;
}

template <size_t N>
void copyText(char (&destination)[N], const String& source) {
  strncpy(destination, source.c_str(), N - 1U);
  destination[N - 1U] = '\0';
}

void setNullable(JsonObject object, const char* name, uint32_t value,
                 uint32_t sentinel) {
  if (value == sentinel) object[name] = nullptr;
  else object[name] = value;
}

bool formatUtc(int64_t unixMs, char* output, size_t capacity) {
  if (unixMs < 0 || output == nullptr || capacity < 25U) return false;
  const time_t seconds = static_cast<time_t>(unixMs / 1000LL);
  tm utc{};
  if (gmtime_r(&seconds, &utc) == nullptr) return false;
  const int written = snprintf(output, capacity,
                               "%04d-%02d-%02dT%02d:%02d:%02d.%03lldZ",
                               utc.tm_year + 1900, utc.tm_mon + 1, utc.tm_mday,
                               utc.tm_hour, utc.tm_min, utc.tm_sec,
                               static_cast<long long>(unixMs % 1000LL));
  return written > 0 && static_cast<size_t>(written) < capacity;
}

}  // namespace

bool Dashboard::begin(ConfigStore& configStore, RadioService& radio,
                      DurableQueue& queue, WifiManager& wifi,
                      BackendWorker& backend, TimeManager& time,
                      CommandStore& commands, OtaManager& ota,
                      const GatewayIdentity& identity,
                      const char* resetReason) {
  configStore_ = &configStore;
  radio_ = &radio;
  queue_ = &queue;
  wifi_ = &wifi;
  backend_ = &backend;
  time_ = &time;
  commands_ = &commands;
  ota_ = &ota;
  identity_ = identity;
  strncpy(resetReason_, resetReason == nullptr ? "unknown" : resetReason,
          sizeof(resetReason_) - 1U);
  registerRoutes();
  server_.begin();
  GTH_LOGI("WEB", "dashboard HTTP server started port=80");
  return true;
}

void Dashboard::loop() {
  server_.handleClient();
  if (rebootAtMs_ != 0U && static_cast<int32_t>(millis() - rebootAtMs_) >= 0) {
    queue_->checkpointCounters(true);
    Serial.flush();
    ESP.restart();
  }
}

void Dashboard::registerRoutes() {
  server_.on("/", HTTP_GET, [this]() {
    server_.sendHeader("Cache-Control", "no-store");
    server_.send_P(200, "text/html; charset=utf-8", kDashboardHtml);
  });
  server_.on("/api/status", HTTP_GET, [this]() { handleStatus(); });
  server_.on("/api/logs", HTTP_GET, [this]() { handleLogs(); });
  server_.on("/api/wifi", HTTP_POST, [this]() { handleWifi(); });
  server_.on("/api/wifi/reconnect", HTTP_POST, [this]() {
    wifi_->requestReconnect();
    sendText(200, "STA reconnect requested");
  });
  server_.on("/api/gateway", HTTP_POST, [this]() { handleGateway(); });
  server_.on("/api/backend", HTTP_POST, [this]() { handleBackend(); });
  server_.on("/api/radio", HTTP_POST, [this]() { handleRadio(); });
  server_.on("/api/radio/restart", HTTP_POST, [this]() {
    const bool restarted = radio_->restart();
    sendText(restarted ? 200 : 503,
             restarted ? "radio restarted" : "radio restart failed");
  });
  server_.on("/api/pair/start", HTTP_POST, [this]() { handlePairStart(); });
  server_.on("/api/pair/confirm", HTTP_POST, [this]() { handlePairConfirm(); });
  server_.on("/api/pair/cancel", HTTP_POST, [this]() { handlePairCancel(); });
  server_.on("/api/pair/manual", HTTP_POST, [this]() { handlePairManual(); });
  server_.on("/api/pair/unpair", HTTP_POST, [this]() { handleUnpair(); });
  server_.on("/api/queue/flush", HTTP_POST, [this]() {
    backend_->flushNow();
    sendText(200, "backend worker awakened; queue records are retained until terminal results");
  });
  server_.on("/api/backend/test", HTTP_POST, [this]() { handleBackendTest(); });
  server_.on("/api/command/maintenance-now", HTTP_POST,
             [this]() { handleEnterMaintenance(); });
  server_.on("/api/command/poll-interval", HTTP_POST,
             [this]() { handlePollIntervalCommand(); });
  server_.on("/api/command/schedule", HTTP_POST,
             [this]() { handleScheduleMaintenance(); });
  server_.on("/api/command/cancel", HTTP_POST,
             [this]() { handleCancelCommand(); });
  server_.on("/api/reboot", HTTP_POST, [this]() {
    if (server_.arg("confirm") != "REBOOT") {
      sendText(422, "confirmation must equal REBOOT");
      return;
    }
    rebootAtMs_ = millis() + 750U;
    sendText(202, "reboot scheduled");
  });
  server_.on("/api/ota", HTTP_POST,
             [this]() { handleOtaComplete(); },
             [this]() { handleOtaUpload(); });
  server_.onNotFound([this]() { sendText(404, "not found"); });
}

void Dashboard::handleStatus() {
  const GatewayConfig& config = configStore_->get();
  const WifiStatus wifi = wifi_->status();
  const RadioDiagnostics radio = radio_->diagnostics();
  const LatestTelemetry latest = radio_->latestTelemetry();
  const PairingCandidate candidate = radio_->pairingCandidate();
  const BackendStatus backend = backend_->status();
  const TimeStatus time = time_->status();
  const QueueOperationalStats queueStats = queue_->operationalStats();
  const size_t queueSize = queue_->size();
  const size_t queueCapacity = queue_->capacity();

  JsonDocument document;
  JsonObject gateway = document["gateway"].to<JsonObject>();
  gateway["firmwareVersion"] = firmware::kVersion;
  gateway["protocolVersion"] = protocol::kVersion;
  gateway["buildFlavor"] = firmware::kBuildFlavor;
  gateway["buildDate"] = firmware::kBuildDate;
  gateway["gitCommit"] = firmware::kGitCommit;
  gateway["hardwareMac"] = identity_.hardwareMac;
  gateway["gatewayId"] = config.gatewayId;
  gateway["gatewayBootSessionId"] = identity_.bootSessionId;
  gateway["uptimeMs"] = static_cast<uint64_t>(esp_timer_get_time()) / 1000ULL;
  gateway["resetReason"] = resetReason_;
  gateway["freeHeapBytes"] = ESP.getFreeHeap();
  gateway["flashBytes"] = ESP.getFlashChipSize();
  gateway["firmwareBytes"] = ESP.getSketchSize();
  gateway["freeOtaSlotBytes"] = ESP.getFreeSketchSpace();
  gateway["filesystemUsedBytes"] = queue_->filesystemUsedBytes();
  gateway["filesystemTotalBytes"] = queue_->filesystemTotalBytes();
  gateway["radioState"] = radio.state;
  gateway["wifiState"] = wifi.connected ? "CONNECTED" : "DISCONNECTED";
  gateway["backendState"] = backend.state;
  gateway["timeState"] = time.trusted ? "SYNCED" : "UNSYNCED";
  gateway["pairedNodeState"] = radio_->paired() ? "PAIRED" : "UNPAIRED";

  JsonObject wifiJson = document["wifi"].to<JsonObject>();
  wifiJson["configuredSsid"] = config.wifiSsid;
  wifiJson["configured"] = wifi.configured;
  wifiJson["connected"] = wifi.connected;
  wifiJson["localIp"] = wifi.connected ? wifi.localIp : "unavailable";
  if (wifi.connected) wifiJson["rssiDbm"] = wifi.rssiDbm;
  else wifiJson["rssiDbm"] = nullptr;
  wifiJson["reconnecting"] = wifi.reconnecting;
  wifiJson["fallbackApActive"] = wifi.fallbackApActive;
  wifiJson["fallbackApSsid"] = wifi.fallbackApSsid;
  wifiJson["fallbackApAddress"] = wifi.fallbackApActive ? "192.168.4.1" : "unavailable";

  JsonObject pairing = document["pairing"].to<JsonObject>();
  pairing["state"] = radio_->paired() ? "PAIRED" : "UNPAIRED";
  char pairedNodeId[build::kNodeIdCapacity]{};
  radio_->pairedNodeId(pairedNodeId, sizeof(pairedNodeId));
  pairing["pairedNodeId"] = pairedNodeId[0] == '\0' ? "unavailable" : pairedNodeId;
  pairing["pairingMode"] = radio_->pairingMode();
  pairing["candidateNodeId"] = candidate.available ? candidate.nodeId : "unavailable";
  if (candidate.available) {
    pairing["candidatePersistentSessionId"] = candidate.persistentSessionId;
    pairing["candidateSequence"] = candidate.sequence;
    pairing["candidateRssiDbm"] = candidate.rssiDbm;
    pairing["candidateSnrDb"] = candidate.snrDb;
  } else {
    pairing["candidatePersistentSessionId"] = nullptr;
    pairing["candidateSequence"] = nullptr;
    pairing["candidateRssiDbm"] = nullptr;
    pairing["candidateSnrDb"] = nullptr;
  }
  if (latest.available) {
    pairing["lastNodeSeenUptimeMs"] = latest.reception.gatewayUptimeMs;
    char lastSeen[32]{};
    if (latest.reception.gatewayTimeTrusted &&
        formatUtc(latest.reception.gatewayReceivedUnixMs, lastSeen,
                  sizeof(lastSeen))) {
      pairing["lastNodeSeenAt"] = lastSeen;
    } else {
      pairing["lastNodeSeenAt"] = nullptr;
    }
  } else {
    pairing["lastNodeSeenUptimeMs"] = nullptr;
    pairing["lastNodeSeenAt"] = nullptr;
  }

  JsonObject telemetry = document["latestTelemetry"].to<JsonObject>();
  if (latest.available) {
    const protocol::TelemetryPacket& packet = latest.packet;
    telemetry["nodeId"] = packet.nodeId;
    telemetry["persistentSessionId"] = packet.persistentSessionId;
    telemetry["sequence"] = packet.sequence;
    telemetry["medianEchoUs"] = packet.medianEchoUs;
    setNullable(telemetry, "rawDistanceMm", packet.rawDistanceMm,
                protocol::kDistanceUnavailable);
    setNullable(telemetry, "acceptedDistanceMm", packet.acceptedDistanceMm,
                protocol::kDistanceUnavailable);
    telemetry["madMm"] = packet.madMm;
    if (packet.temperatureCentiC == protocol::kTemperatureUnavailable)
      telemetry["temperatureC"] = nullptr;
    else telemetry["temperatureC"] = packet.temperatureCentiC / 100.0F;
    if (packet.humidityCentiPercent == protocol::kHumidityUnavailable)
      telemetry["humidityPercent"] = nullptr;
    else telemetry["humidityPercent"] = packet.humidityCentiPercent / 100.0F;
    telemetry["batteryMv"] = packet.batteryMv;
    telemetry["samples"] = String(packet.validSamples) + "/" + packet.totalSamples;
    telemetry["filterState"] = protocol::filterStateName(packet.filterState);
    telemetry["filterStateCode"] = static_cast<uint8_t>(packet.filterState);
    telemetry["qualityFlags"] = packet.qualityFlags;
    telemetry["healthFlags"] = packet.healthFlags;
    telemetry["bootReason"] = protocol::bootReasonName(packet.bootReason);
    telemetry["rtcState"] = protocol::rtcStateName(packet.rtcState);
    telemetry["rtcUnixTime"] = packet.rtcState == protocol::RtcState::kValid
                                   ? packet.rtcUnixTime
                                   : 0U;
    telemetry["pollIntervalMinutes"] = packet.pollIntervalMinutes;
    telemetry["scheduleState"] = protocol::scheduleStateName(packet.scheduleState);
    telemetry["scheduledMaintenanceUnix"] = packet.scheduledMaintenanceUnix;
    telemetry["lastCommandId"] = packet.lastCommandId;
    telemetry["lastCommandType"] = protocol::commandTypeName(packet.lastCommandType);
    telemetry["lastCommandResult"] = protocol::commandResultName(packet.lastCommandResult);
    if (packet.referenceDistanceMm == 0U) {
      telemetry["referenceDistanceMm"] = nullptr;
      telemetry["calibration"] = "missing";
    } else {
      telemetry["referenceDistanceMm"] = packet.referenceDistanceMm;
      telemetry["calibration"] = "configured";
    }
  } else {
    telemetry["state"] = "unavailable";
  }

  JsonObject radioJson = document["radio"].to<JsonObject>();
  radioJson["state"] = radio.state;
  radioJson["ready"] = radio.ready;
  radioJson["lastCode"] = radio.lastCode;
  JsonObject radioConfig = radioJson["configuration"].to<JsonObject>();
  radioConfig["frequencyMhz"] = radio.config.frequencyMhz;
  radioConfig["bandwidthKhz"] = radio.config.bandwidthKhz;
  radioConfig["spreadingFactor"] = radio.config.spreadingFactor;
  radioConfig["codingRateDenominator"] = radio.config.codingRateDenominator;
  radioConfig["txPowerDbm"] = radio.config.txPowerDbm;
  radioConfig["syncWord"] = radio.config.syncWord;
  radioJson["receivedPackets"] = radio.receivedPackets;
  radioJson["validProtocolV3Packets"] = radio.processing.validProtocolPackets;
  radioJson["crcErrors"] = radio.crcErrors;
  radioJson["decodeErrors"] = radio.processing.decodeErrors;
  radioJson["unknownNodePackets"] = radio.processing.unknownNodePackets;
  radioJson["duplicates"] = radio.processing.duplicates;
  radioJson["ackSent"] = radio.processing.ackSent;
  radioJson["ackFailures"] = radio.processing.ackFailures;
  radioJson["commandResultsConfirmed"] = radio.processing.commandResultsConfirmed;
  radioJson["commandResultsIgnored"] = radio.processing.commandResultsIgnored;
  radioJson["lastRssiDbm"] = radio.lastRssiDbm;
  radioJson["lastSnrDb"] = radio.lastSnrDb;
  radioJson["lastFrequencyErrorHz"] = radio.lastFrequencyErrorHz;
  radioJson["lastPacketLength"] = radio.lastPacketLength;
  radioJson["lastQueueWriteLatencyUs"] = radio.processing.lastQueueWriteUs;
  radioJson["lastDurableEnqueueLatencyUs"] =
      radio.processing.lastRxToDurableEnqueueUs;
  radioJson["lastRxToAckStartLatencyUs"] = radio.processing.lastRxToAckStartUs;
  radioJson["lastRxToAckCompleteLatencyUs"] = radio.processing.lastRxToAckCompleteUs;

  JsonObject queue = document["queue"].to<JsonObject>();
  queue["queuedRecords"] = queueSize;
  queue["capacityRecords"] = queueCapacity;
  queue["utilizationPercent"] = queueCapacity == 0U ? 0.0F : 100.0F * queueSize / queueCapacity;
  queue["recordsUploaded"] = queueStats.recordsUploaded;
  queue["uploadRetries"] = queueStats.uploadRetries;
  queue["recordsDeduplicated"] = queueStats.recordsDeduplicated;
  queue["recordsPermanentlyRejected"] = queueStats.recordsPermanentlyRejected;
  queue["recordsDroppedOldest"] = queueStats.recordsDroppedOldest;
  queue["filesystemErrors"] = queueStats.filesystemErrors;
  std::vector<QueueRecord> oldest;
  if (queueSize > 0U && queue_->peek(1, oldest) && !oldest.empty()) {
    int64_t ageMs = -1;
    const TrustedTimeSnapshot current = time_->now();
    if (current.trusted && oldest[0].reception.gatewayTimeTrusted) {
      ageMs = current.unixMs - oldest[0].reception.gatewayReceivedUnixMs;
    } else if (oldest[0].reception.gatewayBootSessionId == identity_.bootSessionId) {
      ageMs = static_cast<int64_t>(esp_timer_get_time() / 1000ULL) -
              static_cast<int64_t>(oldest[0].reception.gatewayUptimeMs);
    }
    if (ageMs >= 0) queue["oldestRecordAgeMs"] = ageMs;
    else queue["oldestRecordAgeMs"] = nullptr;
  } else queue["oldestRecordAgeMs"] = nullptr;

  JsonObject backendJson = document["backend"].to<JsonObject>();
  backendJson["state"] = backend.state;
  backendJson["baseUrl"] = config.backendBaseUrl;
  backendJson["credentialConfigured"] = backend.credentialConfigured;
  backendJson["transport"] = backend.tlsEndpoint ? "HTTPS_VALIDATED" : "LOCAL_HTTP";
  backendJson["batchSize"] = config.backendBatchSize;
  backendJson["timeoutMs"] = config.backendHttpTimeoutMs;
  backendJson["lastAttemptUptimeMs"] = backend.lastAttemptUptimeMs;
  backendJson["lastHttpStatus"] = backend.lastHttpStatus;
  if (backend.lastSuccessUptimeMs > 0U)
    backendJson["lastSuccessUptimeMs"] = backend.lastSuccessUptimeMs;
  else
    backendJson["lastSuccessUptimeMs"] = nullptr;
  if (backend.lastSuccessUnixMs >= 0) backendJson["lastSuccessUnixMs"] = backend.lastSuccessUnixMs;
  else backendJson["lastSuccessUnixMs"] = nullptr;
  backendJson["lastError"] = backend.lastError[0] == '\0' ? "none" : backend.lastError;
  backendJson["currentBackoffMs"] = backend.currentBackoffMs;

  JsonObject timeJson = document["time"].to<JsonObject>();
  timeJson["state"] = time.trusted ? "SYNCED" : "UNSYNCED";
  char currentUtc[32]{};
  if (time.trusted && formatUtc(time.currentUnixMs, currentUtc,
                                sizeof(currentUtc))) {
    timeJson["currentUtc"] = currentUtc;
    timeJson["currentUtcUnixMs"] = time.currentUnixMs;
  } else {
    timeJson["currentUtc"] = nullptr;
    timeJson["currentUtcUnixMs"] = nullptr;
  }

  const GatewayCommand command = commands_->current();
  JsonObject commandJson = document["command"].to<JsonObject>();
  if (command.commandId == 0U) commandJson["commandId"] = nullptr;
  else commandJson["commandId"] = command.commandId;
  commandJson["type"] = protocol::commandTypeName(command.type);
  commandJson["state"] = commandStateName(command.state);
  if (command.pollIntervalMinutes == 0U) commandJson["pollIntervalMinutes"] = nullptr;
  else commandJson["pollIntervalMinutes"] = command.pollIntervalMinutes;
  if (command.scheduledMaintenanceUnix == 0U)
    commandJson["scheduledMaintenanceUnix"] = nullptr;
  else commandJson["scheduledMaintenanceUnix"] = command.scheduledMaintenanceUnix;
  if (command.createdUnixMs >= 0) commandJson["createdUnixMs"] = command.createdUnixMs;
  else commandJson["createdUnixMs"] = nullptr;
  if (command.lastSentUnixMs >= 0) commandJson["lastSentUnixMs"] = command.lastSentUnixMs;
  else commandJson["lastSentUnixMs"] = nullptr;
  commandJson["sendCount"] = command.sendCount;
  commandJson["result"] = protocol::commandResultName(command.result);
  if (command.effectivePollIntervalMinutes == 0U)
    commandJson["effectivePollIntervalMinutes"] = nullptr;
  else commandJson["effectivePollIntervalMinutes"] =
      command.effectivePollIntervalMinutes;
  if (command.effectiveMaintenanceUnix == 0U)
    commandJson["effectiveMaintenanceUnix"] = nullptr;
  else commandJson["effectiveMaintenanceUnix"] = command.effectiveMaintenanceUnix;
  commandJson["gatewayTimeTrusted"] = time.trusted;
  uint8_t knownPollMinutes = latest.available ? latest.packet.pollIntervalMinutes : 0U;
  if (command.state == CommandState::kConfirmed &&
      command.type == protocol::CommandType::kSetPollIntervalMinutes &&
      command.effectivePollIntervalMinutes != 0U &&
      (command.result == protocol::CommandResultCode::kApplied ||
       command.result == protocol::CommandResultCode::kAlreadyApplied)) {
    knownPollMinutes = command.effectivePollIntervalMinutes;
  }
  commandJson["currentKnownPollIntervalMinutes"] = knownPollMinutes;
  char lastSyncUtc[32]{};
  if (time.lastSyncUnixMs >= 0 &&
      formatUtc(time.lastSyncUnixMs, lastSyncUtc, sizeof(lastSyncUtc))) {
    timeJson["lastSyncAt"] = lastSyncUtc;
    timeJson["lastSyncUnixMs"] = time.lastSyncUnixMs;
  } else {
    timeJson["lastSyncAt"] = nullptr;
    timeJson["lastSyncUnixMs"] = nullptr;
  }

  JsonObject ota = document["ota"].to<JsonObject>();
  ota["uploadInProgress"] = ota_->uploadInProgress();
  ota["runningPartition"] = ota_->runningPartition();
  ota["imageState"] = ota_->imageStateName();
  ota["pendingVerification"] = ota_->pendingVerification();
  ota["partitionLayoutSane"] = ota_->partitionLayoutSane();
  ota["lastStatus"] = ota_->lastStatus();

  String body;
  serializeJson(document, body);
  server_.sendHeader("Cache-Control", "no-store");
  server_.send(200, "application/json", body);
}

void Dashboard::handleLogs() {
  // The bounded log ring is about 20 KiB.  WebServer handlers execute on the
  // Arduino loop task, whose stack is much smaller, so the snapshot must live
  // on the heap.  Keeping it on the stack corrupts adjacent RAM before the
  // first mutex acquisition when the dashboard polls this endpoint.
  std::unique_ptr<LogEntry[]> entries(
      new (std::nothrow) LogEntry[build::kLogCapacity]{});
  if (!entries) {
    GTH_LOGE("WEB", "log snapshot allocation failed");
    sendText(503, "log snapshot unavailable");
    return;
  }
  const size_t count = gLogger.snapshot(entries.get(), build::kLogCapacity);
  JsonDocument document;
  JsonArray array = document["entries"].to<JsonArray>();
  for (size_t index = 0; index < count; ++index) {
    JsonObject entry = array.add<JsonObject>();
    entry["uptimeMs"] = entries[index].uptimeMs;
    entry["level"] = Logger::levelName(entries[index].level);
    entry["subsystem"] = entries[index].subsystem;
    entry["message"] = entries[index].message;
  }
  String body;
  serializeJson(document, body);
  server_.sendHeader("Cache-Control", "no-store");
  server_.send(200, "application/json", body);
}

void Dashboard::handleWifi() {
  GatewayConfig candidate = configStore_->get();
  copyText(candidate.wifiSsid, server_.arg("ssid"));
  if (server_.arg("clearPassword") == "true") candidate.wifiPassword[0] = '\0';
  else if (server_.hasArg("password") && !server_.arg("password").isEmpty())
    copyText(candidate.wifiPassword, server_.arg("password"));
  const ConfigValidationResult validation = validateConfig(candidate);
  if (!validation) { sendText(422, validation.message); return; }
  if (!configStore_->save(candidate)) { sendText(500, "NVS configuration write failed"); return; }
  wifi_->applyConfig(candidate);
  sendText(200, "Wi-Fi configuration saved; reconnect requested");
}

void Dashboard::handleGateway() {
  GatewayConfig candidate = configStore_->get();
  copyText(candidate.gatewayId, server_.arg("gatewayId"));
  const ConfigValidationResult validation = validateConfig(candidate);
  if (!validation) { sendText(422, validation.message); return; }
  if (!configStore_->save(candidate)) { sendText(500, "NVS configuration write failed"); return; }
  identity_ = identityFor(candidate);
  backend_->applyConfig(candidate, identity_);
  sendText(200, "logical Gateway ID saved");
}

void Dashboard::handleBackend() {
  GatewayConfig candidate = configStore_->get();
  copyText(candidate.backendBaseUrl, server_.arg("baseUrl"));
  if (server_.arg("clearToken") == "true") candidate.backendBearerToken[0] = '\0';
  else if (server_.hasArg("token") && !server_.arg("token").isEmpty())
    copyText(candidate.backendBearerToken, server_.arg("token"));
  uint32_t value = 0;
  if (!parseUnsigned(server_.arg("batchSize"), value) || value > UINT8_MAX) {
    sendText(422, "invalid batch size"); return;
  }
  candidate.backendBatchSize = static_cast<uint8_t>(value);
  if (!parseUnsigned(server_.arg("timeoutMs"), candidate.backendHttpTimeoutMs)) {
    sendText(422, "invalid HTTP timeout"); return;
  }
  const ConfigValidationResult validation = validateConfig(candidate);
  if (!validation) { sendText(422, validation.message); return; }
  if (!configStore_->save(candidate)) { sendText(500, "NVS configuration write failed"); return; }
  backend_->applyConfig(candidate, identityFor(candidate));
  backend_->flushNow();
  sendText(200, "backend configuration saved; token value remains masked");
}

void Dashboard::handleRadio() {
  const GatewayConfig previous = configStore_->get();
  GatewayConfig candidate = previous;
  uint32_t unsignedValue = 0;
  int32_t signedValue = 0;
  if (!parseFloat(server_.arg("frequencyMhz"), candidate.radio.frequencyMhz) ||
      !parseFloat(server_.arg("bandwidthKhz"), candidate.radio.bandwidthKhz) ||
      !parseUnsigned(server_.arg("spreadingFactor"), unsignedValue) || unsignedValue > UINT8_MAX) {
    sendText(422, "invalid radio numeric value"); return;
  }
  candidate.radio.spreadingFactor = static_cast<uint8_t>(unsignedValue);
  if (!parseUnsigned(server_.arg("codingRateDenominator"), unsignedValue) || unsignedValue > UINT8_MAX) {
    sendText(422, "invalid coding rate"); return;
  }
  candidate.radio.codingRateDenominator = static_cast<uint8_t>(unsignedValue);
  if (!parseSigned(server_.arg("txPowerDbm"), signedValue) || signedValue < INT8_MIN || signedValue > INT8_MAX) {
    sendText(422, "invalid TX power"); return;
  }
  candidate.radio.txPowerDbm = static_cast<int8_t>(signedValue);
  if (!parseUnsigned(server_.arg("syncWord"), unsignedValue) || unsignedValue > UINT8_MAX) {
    sendText(422, "invalid sync word"); return;
  }
  candidate.radio.syncWord = static_cast<uint8_t>(unsignedValue);
  const ConfigValidationResult validation = validateConfig(candidate);
  if (!validation) { sendText(422, validation.message); return; }
  if (!radio_->applyConfig(candidate.radio)) {
    sendText(422, "SX1278 rejected candidate; previous radio configuration restored"); return;
  }
  if (!configStore_->save(candidate)) {
    (void)radio_->applyConfig(previous.radio);
    sendText(500, "NVS save failed; previous radio configuration restored"); return;
  }
  sendText(200, "radio configuration applied and persisted");
}

void Dashboard::handlePairStart() {
  const bool started = radio_->startPairing();
  sendText(started ? 200 : 409,
           started ? "pairing discovery started; candidates are not queued or ACKed"
                   : "unpair the current Node before starting discovery");
}

void Dashboard::handlePairConfirm() {
  char nodeId[build::kNodeIdCapacity]{};
  if (!radio_->confirmPairing(nodeId, sizeof(nodeId))) {
    sendText(409, "no pairing candidate is available"); return;
  }
  if (!savePairedNode(nodeId)) {
    radio_->unpair();
    sendText(500, "NVS pairing save failed; Gateway remains unpaired"); return;
  }
  GTH_LOGI("PAIR", "confirmed Node pairing node=%s", nodeId);
  sendText(200, "Node pairing confirmed; production capture begins with the next packet");
}

void Dashboard::handlePairCancel() {
  (void)radio_->cancelPairing();
  sendText(200, "pairing discovery cancelled");
}

void Dashboard::handlePairManual() {
  const String nodeId = server_.arg("nodeId");
  if (!protocol::nodeIdValid(nodeId.c_str())) { sendText(422, "invalid Node ID"); return; }
  const GatewayConfig previous = configStore_->get();
  GatewayConfig candidate = previous;
  copyText(candidate.pairedNodeId, nodeId);
  if (!configStore_->save(candidate)) { sendText(500, "NVS pairing save failed"); return; }
  if (!radio_->pairManual(nodeId.c_str())) {
    (void)configStore_->save(previous);
    sendText(500, "pairing state update failed"); return;
  }
  GTH_LOGI("PAIR", "manual Node pairing node=%s", nodeId.c_str());
  sendText(200, "Node ID paired manually");
}

void Dashboard::handleUnpair() {
  if (server_.arg("confirm") != "UNPAIR") { sendText(422, "confirmation must equal UNPAIR"); return; }
  GatewayConfig candidate = configStore_->get();
  candidate.pairedNodeId[0] = '\0';
  if (!configStore_->save(candidate)) { sendText(500, "NVS unpair write failed"); return; }
  radio_->unpair();
  GTH_LOGW("PAIR", "Gateway unpaired by dashboard operator");
  sendText(200, "Gateway is now UNPAIRED; telemetry will not be queued or ACKed");
}

void Dashboard::handleBackendTest() {
  int status = 0;
  char error[128]{};
  if (backend_->testConnection(status, error, sizeof(error))) {
    sendText(200, "backend connectivity, TLS/authentication, and schema compatibility passed");
  } else {
    char response[180]{};
    snprintf(response, sizeof(response), "backend test failed HTTP=%d error=%s",
             status, error[0] == '\0' ? "authentication or compatibility failure" : error);
    sendText(503, response);
  }
}

void Dashboard::handleEnterMaintenance() {
  if (server_.arg("confirm") != "ENTER") {
    sendText(422, "confirmation must equal ENTER");
    return;
  }
  if (!radio_->paired()) {
    sendText(409, "no production Node is paired");
    return;
  }
  const TrustedTimeSnapshot now = time_->now();
  uint32_t commandId = 0;
  if (!commands_->create(protocol::CommandType::kEnterMaintenanceNow, 0U, 0U,
                         now.trusted ? now.unixMs : -1, commandId)) {
    sendText(409, commands_->lastError());
    return;
  }
  GTH_LOGI("COMMAND", "queued id=%lu type=ENTER_MAINTENANCE_NOW",
           static_cast<unsigned long>(commandId));
  sendText(202, "ENTER_MAINTENANCE_NOW queued for the next telemetry ACK");
}

void Dashboard::handlePollIntervalCommand() {
  if (!radio_->paired()) {
    sendText(409, "no production Node is paired");
    return;
  }
  uint32_t minutes = 0;
  if (!parseUnsigned(server_.arg("minutes"), minutes) ||
      minutes < 1U || minutes > 255U) {
    sendText(422, "poll interval must be 1-255 minutes");
    return;
  }
  const TrustedTimeSnapshot now = time_->now();
  uint32_t commandId = 0;
  if (!commands_->create(protocol::CommandType::kSetPollIntervalMinutes,
                         static_cast<uint8_t>(minutes), 0U,
                         now.trusted ? now.unixMs : -1, commandId)) {
    sendText(409, commands_->lastError());
    return;
  }
  GTH_LOGI("COMMAND", "queued id=%lu type=SET_POLL_INTERVAL_MINUTES value=%lu",
           static_cast<unsigned long>(commandId), static_cast<unsigned long>(minutes));
  sendText(202, "poll-interval command queued for the next telemetry ACK");
}

void Dashboard::handleScheduleMaintenance() {
  if (!radio_->paired()) {
    sendText(409, "no production Node is paired");
    return;
  }
  const TrustedTimeSnapshot now = time_->now();
  if (!now.trusted || now.unixMs < 0) {
    sendText(409, "Gateway NTP time is untrusted; scheduling is disabled");
    return;
  }
  uint32_t target = 0;
  if (!parseUnsigned(server_.arg("targetUnix"), target)) {
    sendText(422, "targetUnix must be UTC Unix seconds");
    return;
  }
  const uint32_t nowSeconds = static_cast<uint32_t>(now.unixMs / 1000LL);
  constexpr uint32_t horizon = 27U * 24U * 60U * 60U;
  if (target % 60U != 0U || target < nowSeconds + 60U ||
      target - nowSeconds > horizon) {
    sendText(422, "target must be minute-aligned, at least 60 seconds ahead, and within 27 days");
    return;
  }
  uint32_t commandId = 0;
  if (!commands_->create(protocol::CommandType::kScheduleMaintenanceAt,
                         0U, target, now.unixMs, commandId)) {
    sendText(409, commands_->lastError());
    return;
  }
  GTH_LOGI("COMMAND", "queued id=%lu type=SCHEDULE_MAINTENANCE_AT utc=%lu",
           static_cast<unsigned long>(commandId), static_cast<unsigned long>(target));
  sendText(202, "scheduled-maintenance command queued for the next telemetry ACK");
}

void Dashboard::handleCancelCommand() {
  if (server_.arg("confirm") != "CANCEL") {
    sendText(422, "confirmation must equal CANCEL");
    return;
  }
  uint32_t commandId = 0;
  if (!parseUnsigned(server_.arg("commandId"), commandId) ||
      !commands_->cancel(commandId)) {
    sendText(409, "matching pending command was not found or could not be persisted");
    return;
  }
  GTH_LOGW("COMMAND", "cancelled id=%lu",
           static_cast<unsigned long>(commandId));
  sendText(200, "pending command cancelled");
}

void Dashboard::handleOtaUpload() {
  HTTPUpload& upload = server_.upload();
  if (upload.status == UPLOAD_FILE_START) {
    otaUploadOkay_ = false;
    ota_->setUploadInProgress(true);
    GTH_LOGW("OTA", "OTA IN PROGRESS file=%s; telemetry capture is not guaranteed",
             upload.filename.c_str());
    if (!Update.begin(UPDATE_SIZE_UNKNOWN, U_FLASH)) Update.printError(Serial);
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    if (!Update.hasError() && Update.write(upload.buf, upload.currentSize) != upload.currentSize)
      Update.printError(Serial);
  } else if (upload.status == UPLOAD_FILE_END) {
    otaUploadOkay_ = !Update.hasError() && Update.end(true);
    if (!otaUploadOkay_) Update.printError(Serial);
    ota_->setUploadInProgress(false);
  } else if (upload.status == UPLOAD_FILE_ABORTED) {
    Update.abort();
    ota_->setUploadInProgress(false);
    GTH_LOGE("OTA", "OTA upload aborted");
  }
}

void Dashboard::handleOtaComplete() {
  ota_->setUploadInProgress(false);
  if (!otaUploadOkay_) { sendText(500, "OTA upload/write verification failed"); return; }
  GTH_LOGI("OTA", "inactive-slot upload complete; reboot scheduled");
  rebootAtMs_ = millis() + 1000U;
  sendText(202, "OTA upload verified; rebooting into PENDING_VERIFY image");
}

void Dashboard::sendText(int status, const char* text) {
  server_.sendHeader("Cache-Control", "no-store");
  server_.send(status, "text/plain; charset=utf-8", text);
}

bool Dashboard::savePairedNode(const char* nodeId) {
  GatewayConfig candidate = configStore_->get();
  strncpy(candidate.pairedNodeId, nodeId, sizeof(candidate.pairedNodeId) - 1U);
  candidate.pairedNodeId[sizeof(candidate.pairedNodeId) - 1U] = '\0';
  return configStore_->save(candidate);
}

GatewayIdentity Dashboard::identityFor(const GatewayConfig& config) const {
  GatewayIdentity identity = identity_;
  strncpy(identity.gatewayId, config.gatewayId, sizeof(identity.gatewayId) - 1U);
  identity.gatewayId[sizeof(identity.gatewayId) - 1U] = '\0';
  return identity;
}

}  // namespace gathra::gateway

#endif  // ARDUINO
