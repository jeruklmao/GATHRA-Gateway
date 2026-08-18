#pragma once

#include <Module.h>
#include <modules/SX127x/SX1278.h>

#include <memory>

#include "gateway_config.hpp"
#include "packet_processor.hpp"
#include "time_manager.hpp"

namespace gathra::gateway {

struct RadioDiagnostics {
  bool ready = false;
  char state[20]{};
  int16_t lastCode = RADIOLIB_ERR_NONE;
  RadioConfig config{};
  uint64_t receivedPackets = 0;
  uint64_t crcErrors = 0;
  PacketProcessingStats processing{};
  float lastRssiDbm = 0.0F;
  float lastSnrDb = 0.0F;
  int32_t lastFrequencyErrorHz = 0;
  uint16_t lastPacketLength = 0;
};

class SystemMonotonicClock final : public MonotonicClock {
 public:
  uint64_t nowUs() const override;
};

class RadioService final : public AckSink {
 public:
  RadioService();
  bool begin(const RadioConfig& config, PairingManager& pairing,
             PacketQueueSink& queue, TimeManager& time,
             uint32_t gatewayBootSessionId);
  bool applyConfig(const RadioConfig& candidate);
  bool restart();
  RadioDiagnostics diagnostics() const;
  LatestTelemetry latestTelemetry() const;

  bool startPairing();
  bool cancelPairing();
  bool confirmPairing(char* pairedNodeId, size_t capacity);
  bool pairManual(const char* nodeId);
  void unpair();
  PairingCandidate pairingCandidate() const;
  bool pairingMode() const;
  bool paired() const;
  void pairedNodeId(char* output, size_t capacity) const;

#ifdef GATHRA_HIL_SYNTHETIC
  bool injectCanonical(uint32_t sequence);
  void suppressNextAck();
  bool exerciseStandbySleepReceive();
#endif

  AckTransmission transmitAck(const protocol::TelemetryPacket& packet) override;

 private:
  static void taskEntry(void* context);
  void run();
  bool configureUnlocked(const RadioConfig& config);
  bool startReceiveUnlocked();
  void handlePhysicalReceive();
#ifdef GATHRA_HIL_SYNTHETIC
  void handleSynthetic(uint32_t sequence);
#endif
  void handleFrame(const ReceivedFrame& frame, bool synthetic);
  static const char* dispositionName(PacketDisposition disposition);

  Module module_;
  SX1278 radio_;
  mutable SemaphoreHandle_t radioMutex_ = nullptr;
  TaskHandle_t task_ = nullptr;
  QueueHandle_t syntheticQueue_ = nullptr;
  PairingManager* pairing_ = nullptr;
  TimeManager* time_ = nullptr;
  std::unique_ptr<PacketProcessor> processor_;
  SystemMonotonicClock clock_{};
  RadioDiagnostics diagnostics_{};
  uint32_t gatewayBootSessionId_ = 0;
  bool spiStarted_ = false;
  enum class Dio0Action : uint8_t { kNone, kReceive, kTransmit };
  Dio0Action dio0Action_ = Dio0Action::kNone;
#ifdef GATHRA_HIL_SYNTHETIC
  bool suppressNextAck_ = false;
#endif
};

}  // namespace gathra::gateway
