#ifdef ARDUINO

#include "config_store.hpp"

namespace gathra::gateway {

bool ConfigStore::begin(const char* derivedGatewayId) {
  if (!preferences_.begin("gathra-gw", false)) return false;
  GatewayConfig loaded{};
  const size_t storedSize = preferences_.getBytesLength("config");
  if (storedSize == sizeof(loaded) &&
      preferences_.getBytes("config", &loaded, sizeof(loaded)) == sizeof(loaded) &&
      validateConfig(loaded)) {
    config_ = loaded;
  } else {
    config_.setDefaults(derivedGatewayId);
    if (!save(config_)) return false;
  }
  healthy_ = true;
  return true;
}

bool ConfigStore::save(const GatewayConfig& config) {
  if (!validateConfig(config)) return false;
  if (preferences_.putBytes("config", &config, sizeof(config)) != sizeof(config)) return false;
  config_ = config;
  return true;
}

}  // namespace gathra::gateway

#endif  // ARDUINO
