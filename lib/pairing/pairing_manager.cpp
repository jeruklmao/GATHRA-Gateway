#include "pairing_manager.hpp"

#include <string.h>

namespace gathra::gateway {

void PairingManager::begin(const char* persistedNodeId) {
  pairedNodeId_[0] = '\0';
  if (protocol::nodeIdValid(persistedNodeId)) {
    strncpy(pairedNodeId_, persistedNodeId, sizeof(pairedNodeId_) - 1U);
  }
  pairingMode_ = false;
  candidate_ = {};
}

bool PairingManager::start() {
  if (paired()) return false;
  pairingMode_ = true;
  candidate_ = {};
  return true;
}

void PairingManager::cancel() {
  pairingMode_ = false;
  candidate_ = {};
}

void PairingManager::observe(const protocol::TelemetryPacket& packet,
                             float rssiDbm, float snrDb, uint64_t uptimeMs) {
  if (!pairingMode_) return;
  candidate_ = {};
  candidate_.available = true;
  strncpy(candidate_.nodeId, packet.nodeId, sizeof(candidate_.nodeId) - 1U);
  candidate_.bootSessionId = packet.bootSessionId;
  candidate_.sequence = packet.sequence;
  candidate_.rssiDbm = rssiDbm;
  candidate_.snrDb = snrDb;
  candidate_.observedUptimeMs = uptimeMs;
}

bool PairingManager::confirmCandidate() {
  if (!pairingMode_ || !candidate_.available ||
      !protocol::nodeIdValid(candidate_.nodeId)) {
    return false;
  }
  strncpy(pairedNodeId_, candidate_.nodeId, sizeof(pairedNodeId_) - 1U);
  pairedNodeId_[sizeof(pairedNodeId_) - 1U] = '\0';
  pairingMode_ = false;
  return true;
}

bool PairingManager::pairManual(const char* nodeId) {
  if (!protocol::nodeIdValid(nodeId)) return false;
  strncpy(pairedNodeId_, nodeId, sizeof(pairedNodeId_) - 1U);
  pairedNodeId_[sizeof(pairedNodeId_) - 1U] = '\0';
  pairingMode_ = false;
  candidate_ = {};
  return true;
}

void PairingManager::unpair() {
  pairedNodeId_[0] = '\0';
  pairingMode_ = false;
  candidate_ = {};
}

bool PairingManager::matches(const char* nodeId) const {
  return paired() && nodeId != nullptr &&
         strncmp(pairedNodeId_, nodeId, sizeof(pairedNodeId_)) == 0;
}

}  // namespace gathra::gateway
