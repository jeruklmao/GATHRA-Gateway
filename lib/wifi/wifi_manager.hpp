#pragma once

#include <Arduino.h>

#include "gateway_config.hpp"

namespace gathra::gateway {

struct WifiStatus {
  bool configured = false;
  bool connected = false;
  char ssid[build::kWifiSsidCapacity]{};
  char localIp[16]{};
  int32_t rssiDbm = 0;
  bool reconnecting = false;
  bool fallbackApActive = false;
  char fallbackApSsid[33]{};
};

class WifiManager {
 public:
  bool begin(const GatewayConfig& config, const char* hardwareMacCompact);
  void applyConfig(const GatewayConfig& config);
  void requestReconnect();
  WifiStatus status() const;

 private:
  static void taskEntry(void* context);
  void run();
  void startFallbackAp();
  void stopFallbackAp();
  void beginSta(const char* ssid, const char* password);

  mutable SemaphoreHandle_t mutex_ = nullptr;
  char ssid_[build::kWifiSsidCapacity]{};
  char password_[build::kWifiPasswordCapacity]{};
  char fallbackApSsid_[33]{};
  uint32_t reconnectIntervalMs_ = 15'000;
  uint32_t fallbackAfterMs_ = 60'000;
  uint32_t apGraceMs_ = 30'000;
  bool reconnectRequested_ = false;
  bool apActive_ = false;
  bool reconnecting_ = false;
  uint32_t disconnectedSinceMs_ = 0;
  uint32_t connectedSinceMs_ = 0;
  uint32_t lastReconnectMs_ = 0;
};

}  // namespace gathra::gateway
