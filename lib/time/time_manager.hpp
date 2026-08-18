#pragma once

#include <stdint.h>

namespace gathra::gateway {

struct TrustedTimeSnapshot {
  bool trusted = false;
  int64_t unixMs = -1;
};

struct TimeStatus {
  bool trusted = false;
  int64_t currentUnixMs = -1;
  int64_t lastSyncUnixMs = -1;
};

class TimeManager {
 public:
  bool begin();
  TrustedTimeSnapshot now() const;
  TimeStatus status() const;

 private:
  static void taskEntry(void* context);
  void run();
  volatile bool trusted_ = false;
  volatile int64_t lastSyncUnixMs_ = -1;
};

}  // namespace gathra::gateway
