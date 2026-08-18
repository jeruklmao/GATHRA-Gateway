#ifdef ARDUINO

#include "radio_service.hpp"

#include <Arduino.h>
#include <SPI.h>
#include <string.h>

#include "board_pins.hpp"
#include "logger.hpp"
#include "protocol.hpp"

namespace gathra::gateway {
namespace {

volatile TaskHandle_t gRadioTask = nullptr;

void IRAM_ATTR radioInterrupt() {
  BaseType_t higherPriorityTaskWoken = pdFALSE;
  TaskHandle_t task = const_cast<TaskHandle_t>(gRadioTask);
  if (task != nullptr) vTaskNotifyGiveFromISR(task, &higherPriorityTaskWoken);
  if (higherPriorityTaskWoken == pdTRUE) portYIELD_FROM_ISR();
}

class Lock {
 public:
  explicit Lock(SemaphoreHandle_t mutex) : mutex_(mutex) {
    locked_ = mutex_ != nullptr && xSemaphoreTake(mutex_, portMAX_DELAY) == pdTRUE;
  }
  ~Lock() { if (locked_) xSemaphoreGive(mutex_); }
  explicit operator bool() const { return locked_; }
 private:
  SemaphoreHandle_t mutex_;
  bool locked_ = false;
};

}  // namespace

uint64_t SystemMonotonicClock::nowUs() const {
  return static_cast<uint64_t>(esp_timer_get_time());
}

RadioService::RadioService()
    : module_(board::kRadioCs, board::kRadioDio0, board::kRadioReset, RADIOLIB_NC),
      radio_(&module_) {}

bool RadioService::begin(const RadioConfig& config, PairingManager& pairing,
                         PacketQueueSink& queue, TimeManager& time,
                         uint32_t gatewayBootSessionId) {
  radioMutex_ = xSemaphoreCreateMutex();
  if (radioMutex_ == nullptr) return false;
  pairing_ = &pairing;
  time_ = &time;
  gatewayBootSessionId_ = gatewayBootSessionId;
  processor_ = std::make_unique<PacketProcessor>(pairing, queue, *this, clock_);
#ifdef GATHRA_HIL_SYNTHETIC
  syntheticQueue_ = xQueueCreate(4, sizeof(uint32_t));
  if (syntheticQueue_ == nullptr) return false;
#endif
  if (!spiStarted_) {
    SPI.begin(board::kRadioSck, board::kRadioMiso, board::kRadioMosi,
              board::kRadioCs);
    spiStarted_ = true;
  }
  // Preserve the persisted desired settings even if initial hardware access
  // fails, so a later dashboard restart retries the intended configuration.
  diagnostics_.config = config;
  const BaseType_t created =
      xTaskCreate(taskEntry, "gth-radio", 6144, this, 4, &task_);
  if (created != pdPASS) return false;
  // Publish the worker before arming DIO0 so even a packet that completes
  // immediately after startReceive() has a task to notify.
  gRadioTask = task_;
  bool radioReady = false;
  {
    Lock lock(radioMutex_);
    radioReady = lock && configureUnlocked(config) && startReceiveUnlocked();
  }
  // Retain the worker and ISR target after a hardware/configuration failure.
  // The dashboard can then recover the same immutable wiring with Restart
  // Radio once the SX1278 becomes available.
  if (!radioReady) return false;
  GTH_LOGI("RADIO", "continuous RX worker started priority=4");
  return true;
}

bool RadioService::applyConfig(const RadioConfig& candidate) {
  Lock lock(radioMutex_);
  if (!lock) return false;
  const RadioConfig previous = diagnostics_.config;
  (void)radio_.standby();
  if (configureUnlocked(candidate) && startReceiveUnlocked()) return true;
  GTH_LOGE("RADIO", "candidate config failed code=%d; rolling back", diagnostics_.lastCode);
  const bool restored = configureUnlocked(previous) && startReceiveUnlocked();
  if (!restored) GTH_LOGE("RADIO", "radio rollback failed code=%d", diagnostics_.lastCode);
  return false;
}

bool RadioService::restart() {
  Lock lock(radioMutex_);
  if (!lock) return false;
  const RadioConfig current = diagnostics_.config;
  (void)radio_.standby();
  return configureUnlocked(current) && startReceiveUnlocked();
}

RadioDiagnostics RadioService::diagnostics() const {
  Lock lock(radioMutex_);
  RadioDiagnostics copy = diagnostics_;
  if (lock && processor_ != nullptr) copy.processing = processor_->stats();
  return copy;
}

LatestTelemetry RadioService::latestTelemetry() const {
  Lock lock(radioMutex_);
  return lock && processor_ != nullptr ? processor_->latest() : LatestTelemetry{};
}

bool RadioService::startPairing() {
  Lock lock(radioMutex_);
  return lock && pairing_ != nullptr && pairing_->start();
}

bool RadioService::cancelPairing() {
  Lock lock(radioMutex_);
  if (!lock || pairing_ == nullptr) return false;
  pairing_->cancel();
  return true;
}

bool RadioService::confirmPairing(char* pairedNodeId, size_t capacity) {
  Lock lock(radioMutex_);
  if (!lock || pairing_ == nullptr || !pairing_->confirmCandidate()) return false;
  if (pairedNodeId != nullptr && capacity > 0U) {
    strncpy(pairedNodeId, pairing_->pairedNodeId(), capacity - 1U);
    pairedNodeId[capacity - 1U] = '\0';
  }
  return true;
}

bool RadioService::pairManual(const char* nodeId) {
  Lock lock(radioMutex_);
  return lock && pairing_ != nullptr && pairing_->pairManual(nodeId);
}

void RadioService::unpair() {
  Lock lock(radioMutex_);
  if (lock && pairing_ != nullptr) pairing_->unpair();
}

PairingCandidate RadioService::pairingCandidate() const {
  Lock lock(radioMutex_);
  return lock && pairing_ != nullptr ? pairing_->candidate() : PairingCandidate{};
}

bool RadioService::pairingMode() const {
  Lock lock(radioMutex_);
  return lock && pairing_ != nullptr && pairing_->pairingMode();
}

bool RadioService::paired() const {
  Lock lock(radioMutex_);
  return lock && pairing_ != nullptr && pairing_->paired();
}

void RadioService::pairedNodeId(char* output, size_t capacity) const {
  if (output == nullptr || capacity == 0U) return;
  output[0] = '\0';
  Lock lock(radioMutex_);
  if (lock && pairing_ != nullptr) {
    strncpy(output, pairing_->pairedNodeId(), capacity - 1U);
    output[capacity - 1U] = '\0';
  }
}

#ifdef GATHRA_HIL_SYNTHETIC
bool RadioService::injectCanonical(uint32_t sequence) {
  if (syntheticQueue_ == nullptr || task_ == nullptr) return false;
  if (xQueueSend(syntheticQueue_, &sequence, 0) != pdTRUE) return false;
  xTaskNotifyGive(task_);
  return true;
}

void RadioService::suppressNextAck() {
  Lock lock(radioMutex_);
  if (lock) suppressNextAck_ = true;
}

bool RadioService::exerciseStandbySleepReceive() {
  Lock lock(radioMutex_);
  if (!lock || !diagnostics_.ready) return false;
  const RadioConfig current = diagnostics_.config;
  diagnostics_.lastCode = radio_.standby();
  if (diagnostics_.lastCode != RADIOLIB_ERR_NONE) return false;
  diagnostics_.lastCode = radio_.sleep();
  if (diagnostics_.lastCode != RADIOLIB_ERR_NONE) return false;
  delay(20);
  return configureUnlocked(current) && startReceiveUnlocked();
}
#endif

AckTransmission RadioService::transmitAck(const protocol::TelemetryPacket& packet) {
  AckTransmission report{};
  uint8_t bytes[build::kRadioPacketCapacity]{};
  size_t length = 0;
  if (!protocol::encodeAck(protocol::makeAck(packet), bytes, sizeof(bytes), length)) {
    report.receiveRestored = startReceiveUnlocked();
    return report;
  }
#ifdef GATHRA_HIL_SYNTHETIC
  if (suppressNextAck_) {
    suppressNextAck_ = false;
    report.attempted = true;
    report.startUs = clock_.nowUs();
    report.completedUs = report.startUs;
    report.receiveRestored = startReceiveUnlocked();
    GTH_LOGW("ACK", "HIL intentionally suppressed one ACK sequence=%lu",
             static_cast<unsigned long>(packet.sequence));
    return report;
  }
#endif
  (void)ulTaskNotifyTake(pdTRUE, 0);
  radio_.clearPacketReceivedAction();
  radio_.setPacketSentAction(radioInterrupt);
  report.attempted = true;
  report.startUs = clock_.nowUs();
  diagnostics_.state[0] = '\0';
  strncpy(diagnostics_.state, "ACK_TX", sizeof(diagnostics_.state) - 1U);
  diagnostics_.lastCode = radio_.startTransmit(bytes, length);
  if (diagnostics_.lastCode == RADIOLIB_ERR_NONE) {
    uint32_t timeoutMs = radio_.getTimeOnAir(length) / 1000U + 1000U;
    if (timeoutMs < 1500U) timeoutMs = 1500U;
    if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(timeoutMs)) > 0U) {
      diagnostics_.lastCode = radio_.finishTransmit();
      report.success = diagnostics_.lastCode == RADIOLIB_ERR_NONE;
    } else {
      diagnostics_.lastCode = RADIOLIB_ERR_TX_TIMEOUT;
      (void)radio_.standby();
    }
  }
  report.completedUs = clock_.nowUs();
  report.receiveRestored = startReceiveUnlocked();
  return report;
}

void RadioService::taskEntry(void* context) {
  static_cast<RadioService*>(context)->run();
}

void RadioService::run() {
  while (true) {
    (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    bool handledSynthetic = false;
#ifdef GATHRA_HIL_SYNTHETIC
    uint32_t sequence = 0;
    while (xQueueReceive(syntheticQueue_, &sequence, 0) == pdTRUE) {
      Lock lock(radioMutex_);
      if (lock) handleSynthetic(sequence);
      handledSynthetic = true;
    }
#endif
    if (!handledSynthetic) {
      Lock lock(radioMutex_);
      if (lock) handlePhysicalReceive();
    }
  }
}

bool RadioService::configureUnlocked(const RadioConfig& config) {
  diagnostics_.state[0] = '\0';
  strncpy(diagnostics_.state, "CONFIGURING", sizeof(diagnostics_.state) - 1U);
  diagnostics_.lastCode = radio_.begin(
      config.frequencyMhz, config.bandwidthKhz, config.spreadingFactor,
      config.codingRateDenominator, config.syncWord, config.txPowerDbm);
  diagnostics_.ready = diagnostics_.lastCode == RADIOLIB_ERR_NONE;
  if (diagnostics_.ready) {
    diagnostics_.lastCode = radio_.setCRC(true);
    diagnostics_.ready = diagnostics_.lastCode == RADIOLIB_ERR_NONE;
  }
  if (diagnostics_.ready) {
    diagnostics_.config = config;
    GTH_LOGI("RADIO", "SX1278 ready %.3f MHz BW %.1f SF%u CR4/%u %d dBm sync=0x%02X CRC=on",
             config.frequencyMhz, config.bandwidthKhz, config.spreadingFactor,
             config.codingRateDenominator, config.txPowerDbm, config.syncWord);
  } else {
    strncpy(diagnostics_.state, "ERROR", sizeof(diagnostics_.state) - 1U);
    GTH_LOGE("RADIO", "SX1278 initialization/configuration failed code=%d",
             diagnostics_.lastCode);
  }
  return diagnostics_.ready;
}

bool RadioService::startReceiveUnlocked() {
  radio_.clearPacketSentAction();
  radio_.setPacketReceivedAction(radioInterrupt);
  (void)ulTaskNotifyTake(pdTRUE, 0);
  diagnostics_.lastCode = radio_.startReceive();
  const bool okay = diagnostics_.lastCode == RADIOLIB_ERR_NONE;
  diagnostics_.ready = okay;
  diagnostics_.state[0] = '\0';
  strncpy(diagnostics_.state, okay ? "RECEIVING" : "RX_ERROR",
          sizeof(diagnostics_.state) - 1U);
  return okay;
}

void RadioService::handlePhysicalReceive() {
  if (!diagnostics_.ready) return;
  ReceivedFrame frame{};
  frame.observedUs = clock_.nowUs();
  ++diagnostics_.receivedPackets;
  const size_t length = radio_.getPacketLength();
  diagnostics_.lastPacketLength = static_cast<uint16_t>(length);
  if (length == 0U || length > build::kRadioPacketCapacity) {
    (void)radio_.standby();
    (void)startReceiveUnlocked();
    GTH_LOGW("RADIO", "invalid received length=%u", static_cast<unsigned>(length));
    return;
  }
  diagnostics_.lastCode = radio_.readData(frame.bytes, length);
  if (diagnostics_.lastCode == RADIOLIB_ERR_CRC_MISMATCH) {
    ++diagnostics_.crcErrors;
    (void)startReceiveUnlocked();
    GTH_LOGW("RADIO", "packet CRC mismatch length=%u", static_cast<unsigned>(length));
    return;
  }
  if (diagnostics_.lastCode != RADIOLIB_ERR_NONE) {
    const int16_t readCode = diagnostics_.lastCode;
    (void)startReceiveUnlocked();
    GTH_LOGW("RADIO", "packet read failed code=%d", readCode);
    return;
  }
  frame.length = length;
  frame.reception.packetLength = static_cast<uint16_t>(length);
  frame.reception.gatewayBootSessionId = gatewayBootSessionId_;
  frame.reception.gatewayUptimeMs = frame.observedUs / 1000ULL;
  frame.reception.rssiDbm = radio_.getRSSI();
  frame.reception.snrDb = radio_.getSNR();
  frame.reception.frequencyErrorHz = static_cast<int32_t>(radio_.getFrequencyError());
  const TrustedTimeSnapshot timestamp = time_->now();
  frame.reception.gatewayTimeTrusted = timestamp.trusted;
  frame.reception.gatewayReceivedUnixMs = timestamp.trusted ? timestamp.unixMs : -1;
  diagnostics_.lastRssiDbm = frame.reception.rssiDbm;
  diagnostics_.lastSnrDb = frame.reception.snrDb;
  diagnostics_.lastFrequencyErrorHz = frame.reception.frequencyErrorHz;
  handleFrame(frame, false);
}

#ifdef GATHRA_HIL_SYNTHETIC
void RadioService::handleSynthetic(uint32_t sequence) {
  static const uint8_t golden[] = {
      0x47, 0x54, 0x01, 0x01, 0x02, 0x4E, 0x31,
      0x01, 0x02, 0x03, 0x04, 0xA0, 0xB0, 0xC0, 0xD0,
      0x00, 0x00, 0x12, 0x34, 0x00, 0x00, 0x02, 0xE4,
      0x00, 0x00, 0x02, 0xE3, 0x00, 0x03, 0xFB, 0x2E,
      0x11, 0xD7, 0x0E, 0x74, 0x07, 0x07, 0x00, 0x00,
      0x03, 0x02, 0x02};
  (void)radio_.standby();
  ReceivedFrame frame{};
  memcpy(frame.bytes, golden, sizeof(golden));
  frame.bytes[11] = static_cast<uint8_t>(sequence >> 24U);
  frame.bytes[12] = static_cast<uint8_t>(sequence >> 16U);
  frame.bytes[13] = static_cast<uint8_t>(sequence >> 8U);
  frame.bytes[14] = static_cast<uint8_t>(sequence);
  frame.length = sizeof(golden);
  frame.observedUs = clock_.nowUs();
  frame.reception.gatewayBootSessionId = gatewayBootSessionId_;
  frame.reception.gatewayUptimeMs = frame.observedUs / 1000ULL;
  frame.reception.rssiDbm = -42.0F;
  frame.reception.snrDb = 9.5F;
  frame.reception.frequencyErrorHz = 0;
  frame.reception.packetLength = sizeof(golden);
  const TrustedTimeSnapshot timestamp = time_->now();
  frame.reception.gatewayTimeTrusted = timestamp.trusted;
  frame.reception.gatewayReceivedUnixMs = timestamp.trusted ? timestamp.unixMs : -1;
  ++diagnostics_.receivedPackets;
  diagnostics_.lastPacketLength = sizeof(golden);
  diagnostics_.lastRssiDbm = frame.reception.rssiDbm;
  diagnostics_.lastSnrDb = frame.reception.snrDb;
  diagnostics_.lastFrequencyErrorHz = 0;
  GTH_LOGI("RADIO", "HIL canonical post-RX injection sequence=%lu",
           static_cast<unsigned long>(sequence));
  handleFrame(frame, true);
}
#endif

void RadioService::handleFrame(const ReceivedFrame& frame, bool synthetic) {
  if (processor_ == nullptr) {
    (void)startReceiveUnlocked();
    return;
  }
  const PacketProcessingResult result = processor_->process(frame);
  if (!result.ack.receiveRestored) (void)startReceiveUnlocked();
  if (result.droppedOldest) {
    GTH_LOGE("QUEUE",
             "capacity/full-storage policy dropped oldest record; newest seq=%lu persisted=%s",
             static_cast<unsigned long>(result.telemetry.sequence),
             result.disposition == PacketDisposition::kDurableEnqueueFailed
                 ? "no"
                 : "yes");
  }
  if (result.decodeStatus == protocol::DecodeStatus::kOk) {
    GTH_LOGI("RADIO", "%sRX node=%s boot=%lu seq=%lu rssi=%.1f snr=%.1f disposition=%s",
             synthetic ? "synthetic " : "", result.telemetry.nodeId,
             static_cast<unsigned long>(result.telemetry.bootSessionId),
             static_cast<unsigned long>(result.telemetry.sequence),
             frame.reception.rssiDbm, frame.reception.snrDb,
             dispositionName(result.disposition));
  } else {
    GTH_LOGW("RADIO", "Protocol v1 decode rejected status=%s length=%u",
             protocol::decodeStatusName(result.decodeStatus),
             static_cast<unsigned>(frame.length));
  }
  if (result.disposition == PacketDisposition::kNewAcknowledged) {
    const PacketProcessingStats& stats = processor_->stats();
    GTH_LOGI("QUEUE", "persisted seq=%lu queue-write=%lluus rx-to-durable=%lluus",
             static_cast<unsigned long>(result.telemetry.sequence),
             static_cast<unsigned long long>(stats.lastQueueWriteUs),
             static_cast<unsigned long long>(stats.lastRxToDurableEnqueueUs));
    GTH_LOGI("ACK", "sent seq=%lu start=%lluus complete=%lluus RX restored=yes",
             static_cast<unsigned long>(result.telemetry.sequence),
             static_cast<unsigned long long>(stats.lastRxToAckStartUs),
             static_cast<unsigned long long>(stats.lastRxToAckCompleteUs));
  } else if (result.disposition == PacketDisposition::kDuplicateAcknowledged) {
    GTH_LOGW("RADIO", "duplicate seq=%lu, re-ACK only",
             static_cast<unsigned long>(result.telemetry.sequence));
  } else if (result.disposition == PacketDisposition::kDurableEnqueueFailed) {
    GTH_LOGE("QUEUE", "durable enqueue failed seq=%lu, packet NOT ACKed",
             static_cast<unsigned long>(result.telemetry.sequence));
  } else if (result.disposition == PacketDisposition::kAckFailed) {
    GTH_LOGE("ACK", "ACK failed seq=%lu code=%d RX restored=%s",
             static_cast<unsigned long>(result.telemetry.sequence),
             diagnostics_.lastCode, result.ack.receiveRestored ? "yes" : "no");
  }
}

const char* RadioService::dispositionName(PacketDisposition disposition) {
  switch (disposition) {
    case PacketDisposition::kDecodeRejected: return "DECODE_REJECTED";
    case PacketDisposition::kPairingCandidate: return "PAIRING_CANDIDATE";
    case PacketDisposition::kUnpairedIgnored: return "UNPAIRED_IGNORED";
    case PacketDisposition::kUnknownNodeIgnored: return "UNKNOWN_NODE_IGNORED";
    case PacketDisposition::kDurableEnqueueFailed: return "ENQUEUE_FAILED";
    case PacketDisposition::kNewAcknowledged: return "NEW_ACKNOWLEDGED";
    case PacketDisposition::kDuplicateAcknowledged: return "DUPLICATE_REACK";
    case PacketDisposition::kAckFailed: return "ACK_FAILED";
  }
  return "UNKNOWN";
}

}  // namespace gathra::gateway

#endif  // ARDUINO
