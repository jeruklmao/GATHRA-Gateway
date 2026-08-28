#pragma once

#include <Preferences.h>

#include "gateway_config.hpp"

namespace gathra::gateway {

class ConfigStore {
 public:
  bool begin(const char* derivedGatewayId);
  bool save(const GatewayConfig& config);
  const GatewayConfig& get() const { return config_; }
  GatewayConfig& mutableConfig() { return config_; }
  void replaceInMemory(const GatewayConfig& config) { config_ = config; }
  bool healthy() const { return healthy_; }
  uint32_t bootCount() const { return bootCount_; }
  bool bootCountHealthy() const { return bootCountHealthy_; }
  bool migratedLegacyConfig() const { return migratedLegacyConfig_; }

 private:
  Preferences preferences_;
  GatewayConfig config_{};
  uint32_t bootCount_ = 0;
  bool healthy_ = false;
  bool bootCountHealthy_ = false;
  bool migratedLegacyConfig_ = false;
};

}  // namespace gathra::gateway
