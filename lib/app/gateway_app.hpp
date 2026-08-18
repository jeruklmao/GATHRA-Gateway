#pragma once

#include <Arduino.h>

#include "backend_worker.hpp"
#include "config_store.hpp"
#include "dashboard.hpp"
#include "durable_queue.hpp"
#include "ota_manager.hpp"
#include "pairing_manager.hpp"
#include "radio_service.hpp"
#include "time_manager.hpp"
#include "wifi_manager.hpp"

namespace gathra::gateway {

class GatewayApp {
 public:
  void begin();
  void loop();

 private:
  void deriveIdentity();
  static const char* resetReasonName();
#ifdef GATHRA_HIL_SYNTHETIC
  void handleHilSerial();
#endif

  char hardwareMac_[18]{};
  char hardwareMacCompact_[13]{};
  char derivedGatewayId_[build::kGatewayIdCapacity]{};
  GatewayIdentity identity_{};
  ConfigStore configStore_{};
  DurableQueue queue_{};
  PairingManager pairing_{};
  TimeManager time_{};
  RadioService radio_{};
  WifiManager wifi_{};
  BackendWorker backend_{};
  OtaManager ota_{};
  Dashboard dashboard_{};
#ifdef GATHRA_HIL_SYNTHETIC
  String serialLine_;
  uint32_t hilSequence_ = 1;
#endif
};

}  // namespace gathra::gateway
