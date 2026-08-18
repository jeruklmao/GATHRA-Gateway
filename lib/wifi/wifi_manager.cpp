#ifdef ARDUINO

#include "wifi_manager.hpp"

#include <ESPmDNS.h>
#include <WiFi.h>
#include <string.h>

#include "build_config.hpp"
#include "logger.hpp"

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

}  // namespace

bool WifiManager::begin(const GatewayConfig& config, const char* hardwareMacCompact) {
  mutex_ = xSemaphoreCreateMutex();
  if (mutex_ == nullptr) return false;
  snprintf(fallbackApSsid_, sizeof(fallbackApSsid_), "GATHRA-GW-%s",
           hardwareMacCompact == nullptr ? "UNKNOWN" : hardwareMacCompact + 6);
  applyConfig(config);
  WiFi.persistent(false);
  WiFi.setSleep(false);
  WiFi.setHostname(build::kMdnsHost);
  disconnectedSinceMs_ = millis();
  if (ssid_[0] == '\0') startFallbackAp(); else beginSta(ssid_, password_);
  return xTaskCreate(taskEntry, "gth-wifi", 4096, this, 1, nullptr) == pdPASS;
}

void WifiManager::applyConfig(const GatewayConfig& config) {
  Lock lock(mutex_);
  if (!lock) return;
  strncpy(ssid_, config.wifiSsid, sizeof(ssid_) - 1U);
  ssid_[sizeof(ssid_) - 1U] = '\0';
  strncpy(password_, config.wifiPassword, sizeof(password_) - 1U);
  password_[sizeof(password_) - 1U] = '\0';
  reconnectIntervalMs_ = config.wifiReconnectIntervalMs;
  fallbackAfterMs_ = config.wifiFallbackAfterMs;
  apGraceMs_ = config.wifiApGraceMs;
  reconnectRequested_ = true;
}

void WifiManager::requestReconnect() {
  Lock lock(mutex_);
  if (lock) reconnectRequested_ = true;
}

WifiStatus WifiManager::status() const {
  Lock lock(mutex_);
  WifiStatus value{};
  if (!lock) return value;
  value.configured = ssid_[0] != '\0';
  value.connected = WiFi.status() == WL_CONNECTED;
  strncpy(value.ssid, ssid_, sizeof(value.ssid) - 1U);
  if (value.connected) {
    strncpy(value.localIp, WiFi.localIP().toString().c_str(), sizeof(value.localIp) - 1U);
    value.rssiDbm = WiFi.RSSI();
  }
  value.reconnecting = reconnecting_;
  value.fallbackApActive = apActive_;
  strncpy(value.fallbackApSsid, fallbackApSsid_, sizeof(value.fallbackApSsid) - 1U);
  return value;
}

void WifiManager::taskEntry(void* context) {
  static_cast<WifiManager*>(context)->run();
}

void WifiManager::run() {
  bool wasConnected = false;
  while (true) {
    char ssid[sizeof(ssid_)]{};
    char password[sizeof(password_)]{};
    uint32_t reconnectIntervalMs = 0;
    uint32_t fallbackAfterMs = 0;
    uint32_t apGraceMs = 0;
    bool reconnectRequested = false;
    {
      Lock lock(mutex_);
      if (lock) {
        memcpy(ssid, ssid_, sizeof(ssid));
        memcpy(password, password_, sizeof(password));
        reconnectIntervalMs = reconnectIntervalMs_;
        fallbackAfterMs = fallbackAfterMs_;
        apGraceMs = apGraceMs_;
        reconnectRequested = reconnectRequested_;
        reconnectRequested_ = false;
      }
    }

    const uint32_t now = millis();
    const bool connected = WiFi.status() == WL_CONNECTED;
    if (connected && !wasConnected) {
      connectedSinceMs_ = now;
      {
        Lock lock(mutex_);
        if (lock) reconnecting_ = false;
      }
      GTH_LOGI("WIFI", "STA connected ssid=%s ip=%s rssi=%d",
               ssid, WiFi.localIP().toString().c_str(), WiFi.RSSI());
      if (MDNS.begin(build::kMdnsHost)) MDNS.addService("http", "tcp", 80);
    } else if (!connected && wasConnected) {
      disconnectedSinceMs_ = now;
      MDNS.end();
      GTH_LOGW("WIFI", "STA connection lost");
    }
    wasConnected = connected;

    if (reconnectRequested) {
      beginSta(ssid, password);
    }
    if (!connected) {
      if (disconnectedSinceMs_ == 0U) disconnectedSinceMs_ = now;
      if (ssid[0] != '\0' && now - lastReconnectMs_ >= reconnectIntervalMs) {
        beginSta(ssid, password);
      }
      if (ssid[0] == '\0' || now - disconnectedSinceMs_ >= fallbackAfterMs) {
        startFallbackAp();
      }
    } else {
      disconnectedSinceMs_ = 0U;
      bool apActive = false;
      {
        Lock lock(mutex_);
        if (lock) apActive = apActive_;
      }
      if (apActive && now - connectedSinceMs_ >= apGraceMs) stopFallbackAp();
    }
    vTaskDelay(pdMS_TO_TICKS(500));
  }
}

void WifiManager::startFallbackAp() {
  {
    Lock lock(mutex_);
    if (!lock || apActive_) return;
  }
  WiFi.mode(WIFI_AP_STA);
  if (WiFi.softAP(fallbackApSsid_, build::kFallbackApPassword)) {
    {
      Lock lock(mutex_);
      if (lock) apActive_ = true;
    }
    GTH_LOGW("WIFI", "fallback AP active ssid=%s ip=%s", fallbackApSsid_,
             WiFi.softAPIP().toString().c_str());
  } else {
    GTH_LOGE("WIFI", "fallback AP start failed");
  }
}

void WifiManager::stopFallbackAp() {
  {
    Lock lock(mutex_);
    if (!lock || !apActive_) return;
  }
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_STA);
  {
    Lock lock(mutex_);
    if (lock) apActive_ = false;
  }
  GTH_LOGI("WIFI", "fallback AP stopped after STA grace period");
}

void WifiManager::beginSta(const char* ssid, const char* password) {
  lastReconnectMs_ = millis();
  if (ssid == nullptr || ssid[0] == '\0') {
    {
      Lock lock(mutex_);
      if (lock) reconnecting_ = false;
    }
    startFallbackAp();
    return;
  }
  bool apActive = false;
  {
    Lock lock(mutex_);
    if (lock) apActive = apActive_;
  }
  if (apActive) WiFi.mode(WIFI_AP_STA); else WiFi.mode(WIFI_STA);
  WiFi.disconnect(false, false);
  WiFi.begin(ssid, password == nullptr ? "" : password);
  {
    Lock lock(mutex_);
    if (lock) reconnecting_ = true;
  }
  if (disconnectedSinceMs_ == 0U) disconnectedSinceMs_ = millis();
  GTH_LOGI("WIFI", "STA connection attempt ssid=%s", ssid);
}

}  // namespace gathra::gateway

#endif  // ARDUINO
