#ifdef ARDUINO

#include "ota_manager.hpp"

#include <Arduino.h>
#include <string.h>

#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "logger.hpp"

// Arduino-ESP32 otherwise validates a PENDING_VERIFY image before setup().
// Deferral lets GatewayApp validate NVS, queue recovery, and partition layout.
extern "C" bool verifyRollbackLater() { return true; }

namespace gathra::gateway {
namespace {

const char* stateName(esp_ota_img_states_t state) {
  switch (state) {
    case ESP_OTA_IMG_NEW: return "NEW";
    case ESP_OTA_IMG_PENDING_VERIFY: return "PENDING_VERIFY";
    case ESP_OTA_IMG_VALID: return "VALID";
    case ESP_OTA_IMG_INVALID: return "INVALID";
    case ESP_OTA_IMG_ABORTED: return "ABORTED";
    case ESP_OTA_IMG_UNDEFINED: return "UNDEFINED";
  }
  return "UNKNOWN";
}

}  // namespace

void OtaManager::begin() {
  const esp_partition_t* running = esp_ota_get_running_partition();
  if (running != nullptr) {
    strncpy(runningPartition_, running->label, sizeof(runningPartition_) - 1U);
    esp_ota_img_states_t state = ESP_OTA_IMG_UNDEFINED;
    if (esp_ota_get_state_partition(running, &state) == ESP_OK) {
      imageStateName_ = stateName(state);
      pendingVerification_ = state == ESP_OTA_IMG_PENDING_VERIFY;
    }
  }
  GTH_LOGI("OTA", "running=%s state=%s rollback-config=%s", runningPartition_,
           imageStateName_,
#ifdef CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE
           "enabled"
#else
           "disabled"
#endif
  );
}

bool OtaManager::partitionLayoutSane() const {
  const esp_partition_t* ota0 = esp_partition_find_first(
      ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, nullptr);
  const esp_partition_t* ota1 = esp_partition_find_first(
      ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_1, nullptr);
  const esp_partition_t* otaData = esp_partition_find_first(
      ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_OTA, nullptr);
  const esp_partition_t* filesystem = esp_partition_find_first(
      ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, "littlefs");
  return ota0 != nullptr && ota1 != nullptr && otaData != nullptr &&
         filesystem != nullptr && ota0->size == ota1->size &&
         ota0->size == 0x160000U && filesystem->size == 0x120000U &&
         filesystem->address + filesystem->size <= 0x400000U;
}

bool OtaManager::completeBootValidation(bool configurationSane, bool queueSane,
                                        bool nvsSane) {
  const bool sane = configurationSane && queueSane && nvsSane && partitionLayoutSane();
  if (!pendingVerification_) {
    lastStatus_ = sane ? "boot checks passed; image was not pending"
                       : "boot checks failed; image was not pending";
    return sane;
  }
#ifdef GATHRA_ROLLBACK_TEST_FAIL
  GTH_LOGE("OTA", "controlled rollback-test image restarting before validation");
  Serial.flush();
  delay(500);
  esp_restart();
#endif
  if (!sane) {
    lastStatus_ = "boot validation failed; requesting rollback";
    GTH_LOGE("OTA", "%s", lastStatus_);
    delay(200);
    esp_ota_mark_app_invalid_rollback_and_reboot();
    return false;
  }
  const esp_err_t result = esp_ota_mark_app_valid_cancel_rollback();
  if (result == ESP_OK) {
    pendingVerification_ = false;
    imageStateName_ = "VALID";
    lastStatus_ = "pending image marked valid after internal boot checks";
    GTH_LOGI("OTA", "%s", lastStatus_);
    return true;
  }
  lastStatus_ = "failed to mark pending image valid";
  GTH_LOGE("OTA", "%s error=%s", lastStatus_, esp_err_to_name(result));
  return false;
}

}  // namespace gathra::gateway

#endif  // ARDUINO
