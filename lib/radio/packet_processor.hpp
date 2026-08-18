#pragma once

#include <stddef.h>
#include <stdint.h>

#include "deduplicator.hpp"
#include "pairing_manager.hpp"
#include "queue_record.hpp"

namespace gathra::gateway {

struct ReceivedFrame {
  uint8_t bytes[build::kRadioPacketCapacity]{};
  size_t length = 0;
  ReceptionMetadata reception{};
  uint64_t observedUs = 0;
};

class PacketQueueSink {
 public:
  virtual ~PacketQueueSink() = default;
  virtual bool containsKey(const TelemetryKey& key) = 0;
  struct EnqueueOutcome {
    bool persisted = false;
    bool droppedOldest = false;
  };
  virtual EnqueueOutcome durablyEnqueue(QueueRecord& record) = 0;
  virtual void noteDuplicate() = 0;
};

struct AckTransmission {
  bool attempted = false;
  bool success = false;
  bool receiveRestored = false;
  uint64_t startUs = 0;
  uint64_t completedUs = 0;
};

class AckSink {
 public:
  virtual ~AckSink() = default;
  virtual AckTransmission transmitAck(const protocol::TelemetryPacket& packet) = 0;
};

class MonotonicClock {
 public:
  virtual ~MonotonicClock() = default;
  virtual uint64_t nowUs() const = 0;
};

enum class PacketDisposition : uint8_t {
  kDecodeRejected,
  kPairingCandidate,
  kUnpairedIgnored,
  kUnknownNodeIgnored,
  kDurableEnqueueFailed,
  kNewAcknowledged,
  kDuplicateAcknowledged,
  kAckFailed,
};

struct PacketProcessingStats {
  uint64_t validProtocolPackets = 0;
  uint64_t decodeErrors = 0;
  uint64_t unknownNodePackets = 0;
  uint64_t duplicates = 0;
  uint64_t ackSent = 0;
  uint64_t ackFailures = 0;
  uint64_t durableEnqueueFailures = 0;
  uint64_t lastQueueWriteUs = 0;
  uint64_t lastRxToDurableEnqueueUs = 0;
  uint64_t lastRxToAckStartUs = 0;
  uint64_t lastRxToAckCompleteUs = 0;
};

struct LatestTelemetry {
  bool available = false;
  protocol::TelemetryPacket packet{};
  ReceptionMetadata reception{};
};

struct PacketProcessingResult {
  PacketDisposition disposition = PacketDisposition::kDecodeRejected;
  protocol::DecodeStatus decodeStatus = protocol::DecodeStatus::kBufferTooSmall;
  protocol::TelemetryPacket telemetry{};
  AckTransmission ack{};
  bool droppedOldest = false;
};

class PacketProcessor {
 public:
  PacketProcessor(PairingManager& pairing, PacketQueueSink& queue,
                  AckSink& ack, const MonotonicClock& clock)
      : pairing_(pairing), queue_(queue), ack_(ack), clock_(clock) {}
  PacketProcessingResult process(const ReceivedFrame& frame);
  const PacketProcessingStats& stats() const { return stats_; }
  const LatestTelemetry& latest() const { return latest_; }

 private:
  PairingManager& pairing_;
  PacketQueueSink& queue_;
  AckSink& ack_;
  const MonotonicClock& clock_;
  PacketProcessingStats stats_{};
  LatestTelemetry latest_{};
};

}  // namespace gathra::gateway
