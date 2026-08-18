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

 private:
  Preferences preferences_;
  GatewayConfig config_{};
  bool healthy_ = false;
};

}  // namespace gathra::gateway
