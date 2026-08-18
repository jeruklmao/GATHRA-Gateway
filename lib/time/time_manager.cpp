#ifdef ARDUINO

#include "time_manager.hpp"

#include <Arduino.h>
#include <esp_sntp.h>
#include <sys/time.h>
#include <time.h>

#include "logger.hpp"

namespace gathra::gateway {
namespace {

portMUX_TYPE gTimeStateMux = portMUX_INITIALIZER_UNLOCKED;
int64_t gLastSntpSyncUnixMs = -1;

void onSntpTimeSync(timeval* value) {
  if (value == nullptr || value->tv_sec < 1'700'000'000) return;
  const int64_t unixMs =
      static_cast<int64_t>(value->tv_sec) * 1000LL + value->tv_usec / 1000LL;
  portENTER_CRITICAL(&gTimeStateMux);
  gLastSntpSyncUnixMs = unixMs;
  portEXIT_CRITICAL(&gTimeStateMux);
}

int64_t lastSntpSyncUnixMs() {
  portENTER_CRITICAL(&gTimeStateMux);
  const int64_t value = gLastSntpSyncUnixMs;
  portEXIT_CRITICAL(&gTimeStateMux);
  return value;
}

}  // namespace

bool TimeManager::begin() {
  sntp_set_time_sync_notification_cb(onSntpTimeSync);
  configTzTime("UTC0", "pool.ntp.org", "time.cloudflare.com", "time.google.com");
  return xTaskCreate(taskEntry, "gth-ntp", 3072, this, 1, nullptr) == pdPASS;
}

TrustedTimeSnapshot TimeManager::now() const {
  TrustedTimeSnapshot snapshot{};
  portENTER_CRITICAL(&gTimeStateMux);
  const bool trusted = trusted_;
  portEXIT_CRITICAL(&gTimeStateMux);
  if (!trusted) return snapshot;
  timeval value{};
  if (gettimeofday(&value, nullptr) != 0 || value.tv_sec < 1'700'000'000) return snapshot;
  snapshot.trusted = true;
  snapshot.unixMs = static_cast<int64_t>(value.tv_sec) * 1000LL + value.tv_usec / 1000LL;
  return snapshot;
}

TimeStatus TimeManager::status() const {
  const TrustedTimeSnapshot current = now();
  portENTER_CRITICAL(&gTimeStateMux);
  const int64_t lastSync = lastSyncUnixMs_;
  portEXIT_CRITICAL(&gTimeStateMux);
  return {current.trusted, current.unixMs, lastSync};
}

void TimeManager::taskEntry(void* context) {
  static_cast<TimeManager*>(context)->run();
}

void TimeManager::run() {
  int64_t observedSync = -1;
  while (true) {
    const int64_t sync = lastSntpSyncUnixMs();
    if (sync >= 0 && sync != observedSync) {
      portENTER_CRITICAL(&gTimeStateMux);
      trusted_ = true;
      lastSyncUnixMs_ = sync;
      portEXIT_CRITICAL(&gTimeStateMux);
      observedSync = sync;
      GTH_LOGI("NTP", "UTC clock synchronized unixMs=%lld",
               static_cast<long long>(sync));
    }
    vTaskDelay(pdMS_TO_TICKS(5000));
  }
}

}  // namespace gathra::gateway

#endif  // ARDUINO
