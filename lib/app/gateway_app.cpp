#ifdef ARDUINO

#include "gateway_app.hpp"

#include <WiFi.h>
#include <esp_system.h>
#include <string.h>

#include "firmware_version.hpp"
#include "logger.hpp"
#include "protocol.hpp"

namespace gathra::gateway {

void GatewayApp::begin() {
  Serial.begin(build::kSerialBaud);
  delay(300);
  (void)gLogger.begin();
  deriveIdentity();
  identity_.bootSessionId = esp_random();
  if (identity_.bootSessionId == 0U) identity_.bootSessionId = 1U;
  strncpy(identity_.hardwareMac, hardwareMac_, sizeof(identity_.hardwareMac) - 1U);
  strncpy(identity_.firmwareVersion, firmware::kVersion,
          sizeof(identity_.firmwareVersion) - 1U);

  GTH_LOGI("APP", "GATHRA Gateway firmware=%s protocol=%u build=%s git=%s built=%s",
           firmware::kVersion, protocol::kVersion, firmware::kBuildFlavor,
           firmware::kGitCommit, firmware::kBuildDate);
  GTH_LOGI("APP", "hardwareMac=%s bootSessionId=%lu reset=%s",
           hardwareMac_, static_cast<unsigned long>(identity_.bootSessionId),
           resetReasonName());
  ota_.begin();

  const bool configReady = configStore_.begin(derivedGatewayId_);
  if (!configReady) GTH_LOGE("APP", "NVS configuration initialization failed");
  if (configReady) {
    GTH_LOGI("APP", "bootCount=%lu config=%s heartbeat=%lus",
             static_cast<unsigned long>(configStore_.bootCount()),
             configStore_.migratedLegacyConfig() ? "migrated-v1" : "current",
             static_cast<unsigned long>(
                 configStore_.get().heartbeatIntervalSeconds));
    if (!configStore_.bootCountHealthy()) {
      GTH_LOGW("APP", "boot count could not be persisted");
    }
  }
  const GatewayConfig& config = configStore_.get();
  strncpy(identity_.gatewayId, config.gatewayId, sizeof(identity_.gatewayId) - 1U);

  const bool queueReady = queue_.begin();
  if (queueReady) {
    const QueueOperationalStats queueStats = queue_.operationalStats();
    GTH_LOGI("QUEUE", "LittleFS queue recovered depth=%u capacity=%u used=%llu/%llu",
             static_cast<unsigned>(queue_.size()),
             static_cast<unsigned>(queue_.capacity()),
             static_cast<unsigned long long>(queue_.filesystemUsedBytes()),
             static_cast<unsigned long long>(queue_.filesystemTotalBytes()));
    if (queueStats.filesystemErrors > 0U ||
        queueStats.recordsDroppedOldest > 0U) {
      GTH_LOGW("QUEUE", "startup recovery errors=%llu dropped-oldest=%llu",
               static_cast<unsigned long long>(queueStats.filesystemErrors),
               static_cast<unsigned long long>(queueStats.recordsDroppedOldest));
    }
  } else {
    GTH_LOGE("QUEUE", "LittleFS mount/recovery failed; telemetry will NOT be ACKed");
  }

  pairing_.begin(config.pairedNodeId);
  bool commandStateFresh = false;
  const bool commandReady = commands_.begin(commandBackend_, commandStateFresh);
  if (!commandReady) {
    GTH_LOGE("COMMAND", "persistent command state initialization failed");
  } else {
    GTH_LOGI("COMMAND", "%s nextId=%lu state=%s",
             commandStateFresh ? "initialized" : "restored",
             static_cast<unsigned long>(commands_.nextCommandId()),
             commandStateName(commands_.current().state));
  }
  const bool timeReady = time_.begin();
  const bool radioReady = radio_.begin(config.radio, pairing_, queue_, time_,
                                       commands_, identity_.bootSessionId);
  if (!radioReady) GTH_LOGE("RADIO", "radio unavailable; dashboard recovery remains active");
  const bool wifiReady = wifi_.begin(config, hardwareMacCompact_);
  if (!wifiReady) GTH_LOGE("WIFI", "Wi-Fi manager initialization failed");
  const bool backendReady = backend_.begin(
      queue_, time_, radio_, wifi_, commands_, config, identity_,
      configStore_.bootCount(), resetReasonName());
  if (!backendReady) GTH_LOGE("BACKEND", "backend worker initialization failed");
  const bool dashboardReady = dashboard_.begin(
      configStore_, radio_, queue_, wifi_, backend_, time_, commands_, ota_, identity_,
      resetReasonName());
  if (!dashboardReady) GTH_LOGE("WEB", "dashboard initialization failed");

  const bool internalStateSane = configReady && queueReady && timeReady && commandReady &&
                                 configStore_.healthy();
  (void)ota_.completeBootValidation(configReady, queueReady,
                                    internalStateSane);
  GTH_LOGI("APP", "initialization complete radio=%s wifi-manager=%s backend-worker=%s web=%s",
           radioReady ? "ready" : "error", wifiReady ? "ready" : "error",
           backendReady ? "ready" : "error", dashboardReady ? "ready" : "error");
#ifdef GATHRA_HIL_SYNTHETIC
  GTH_LOGW("APP", "HIL-only serial diagnostics enabled: HIL_PAIR, HIL_INJECT [seq], HIL_SUPPRESS_ACK_ONCE, HIL_RADIO_CYCLE");
#endif
}

void GatewayApp::loop() {
  dashboard_.loop();
#ifdef GATHRA_HIL_SYNTHETIC
  handleHilSerial();
#endif
  delay(2);
}

void GatewayApp::deriveIdentity() {
  uint8_t bytes[6]{};
  esp_read_mac(bytes, ESP_MAC_WIFI_STA);
  snprintf(hardwareMac_, sizeof(hardwareMac_), "%02X:%02X:%02X:%02X:%02X:%02X",
           bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], bytes[5]);
  snprintf(hardwareMacCompact_, sizeof(hardwareMacCompact_), "%02X%02X%02X%02X%02X%02X",
           bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], bytes[5]);
  snprintf(derivedGatewayId_, sizeof(derivedGatewayId_), "GTH-GW-%s",
           hardwareMacCompact_);
}

const char* GatewayApp::resetReasonName() {
  switch (esp_reset_reason()) {
    case ESP_RST_UNKNOWN: return "UNKNOWN";
    case ESP_RST_POWERON: return "POWER_ON";
    case ESP_RST_EXT: return "EXTERNAL";
    case ESP_RST_SW: return "SOFTWARE";
    case ESP_RST_PANIC: return "PANIC";
    case ESP_RST_INT_WDT: return "INT_WDT";
    case ESP_RST_TASK_WDT: return "TASK_WDT";
    case ESP_RST_WDT: return "OTHER_WDT";
    case ESP_RST_DEEPSLEEP: return "DEEP_SLEEP";
    case ESP_RST_BROWNOUT: return "BROWNOUT";
    case ESP_RST_SDIO: return "SDIO";
  }
  return "UNKNOWN";
}

#ifdef GATHRA_HIL_SYNTHETIC
void GatewayApp::handleHilSerial() {
  while (Serial.available() > 0) {
    const char value = static_cast<char>(Serial.read());
    if (value == '\r') continue;
    if (value != '\n') {
      if (serialLine_.length() < 80U) serialLine_ += value;
      continue;
    }
    serialLine_.trim();
    if (serialLine_ == "HIL_PAIR") {
      GatewayConfig candidate = configStore_.get();
      strncpy(candidate.pairedNodeId, "N1", sizeof(candidate.pairedNodeId) - 1U);
      if (configStore_.save(candidate) && radio_.pairManual("N1"))
        GTH_LOGI("PAIR", "HIL paired canonical Node N1");
      else GTH_LOGE("PAIR", "HIL pairing failed");
    } else if (serialLine_ == "HIL_UNPAIR") {
      GatewayConfig candidate = configStore_.get();
      candidate.pairedNodeId[0] = '\0';
      if (configStore_.save(candidate)) radio_.unpair();
      GTH_LOGW("PAIR", "HIL unpaired Gateway");
    } else if (serialLine_.startsWith("HIL_INJECT")) {
      const int space = serialLine_.indexOf(' ');
      uint32_t sequence = hilSequence_++;
      if (space > 0) {
        const unsigned long parsed = strtoul(serialLine_.substring(space + 1).c_str(), nullptr, 10);
        sequence = static_cast<uint32_t>(parsed);
      }
      if (!radio_.injectCanonical(sequence)) GTH_LOGE("RADIO", "HIL injection queue full");
    } else if (serialLine_ == "HIL_SUPPRESS_ACK_ONCE") {
      radio_.suppressNextAck();
      GTH_LOGW("ACK", "next ACK will be suppressed by HIL command");
    } else if (serialLine_ == "HIL_RADIO_CYCLE") {
      const bool okay = radio_.exerciseStandbySleepReceive();
      GTH_LOGI("RADIO", "HIL standby/sleep/reconfigure/RX cycle %s",
               okay ? "passed" : "failed");
    } else if (!serialLine_.isEmpty()) {
      GTH_LOGW("APP", "unknown HIL command");
    }
    serialLine_ = "";
  }
}
#endif

}  // namespace gathra::gateway

#endif  // ARDUINO
