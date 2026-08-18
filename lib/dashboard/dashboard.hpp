#pragma once

#include <WebServer.h>

#include "backend_worker.hpp"
#include "config_store.hpp"
#include "durable_queue.hpp"
#include "ota_manager.hpp"
#include "radio_service.hpp"
#include "time_manager.hpp"
#include "wifi_manager.hpp"

namespace gathra::gateway {

class Dashboard {
 public:
  bool begin(ConfigStore& configStore, RadioService& radio,
             DurableQueue& queue, WifiManager& wifi,
             BackendWorker& backend, TimeManager& time,
             OtaManager& ota, const GatewayIdentity& identity,
             const char* resetReason);
  void loop();

 private:
  void registerRoutes();
  void handleStatus();
  void handleLogs();
  void handleWifi();
  void handleGateway();
  void handleBackend();
  void handleRadio();
  void handlePairStart();
  void handlePairConfirm();
  void handlePairCancel();
  void handlePairManual();
  void handleUnpair();
  void handleBackendTest();
  void handleOtaUpload();
  void handleOtaComplete();
  void sendText(int status, const char* text);
  bool savePairedNode(const char* nodeId);
  GatewayIdentity identityFor(const GatewayConfig& config) const;

  WebServer server_{80};
  ConfigStore* configStore_ = nullptr;
  RadioService* radio_ = nullptr;
  DurableQueue* queue_ = nullptr;
  WifiManager* wifi_ = nullptr;
  BackendWorker* backend_ = nullptr;
  TimeManager* time_ = nullptr;
  OtaManager* ota_ = nullptr;
  GatewayIdentity identity_{};
  char resetReason_[32]{};
  uint32_t rebootAtMs_ = 0;
  bool otaUploadOkay_ = false;
};

}  // namespace gathra::gateway
