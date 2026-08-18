#pragma once

#include <stdint.h>

#include "build_config.hpp"
#include "protocol.hpp"

namespace gathra::gateway {

struct PairingCandidate {
  bool available = false;
  char nodeId[build::kNodeIdCapacity]{};
  uint32_t bootSessionId = 0;
  uint32_t sequence = 0;
  float rssiDbm = 0.0F;
  float snrDb = 0.0F;
  uint64_t observedUptimeMs = 0;
};

class PairingManager {
 public:
  void begin(const char* persistedNodeId);
  bool start();
  void cancel();
  void observe(const protocol::TelemetryPacket& packet, float rssiDbm,
               float snrDb, uint64_t uptimeMs);
  bool confirmCandidate();
  bool pairManual(const char* nodeId);
  void unpair();

  bool pairingMode() const { return pairingMode_; }
  bool paired() const { return pairedNodeId_[0] != '\0'; }
  bool matches(const char* nodeId) const;
  const char* pairedNodeId() const { return pairedNodeId_; }
  const PairingCandidate& candidate() const { return candidate_; }

 private:
  char pairedNodeId_[build::kNodeIdCapacity]{};
  bool pairingMode_ = false;
  PairingCandidate candidate_{};
};

}  // namespace gathra::gateway
