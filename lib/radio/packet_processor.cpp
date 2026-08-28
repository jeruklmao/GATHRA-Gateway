#include "packet_processor.hpp"

#include <string.h>

namespace gathra::gateway {

PacketProcessingResult PacketProcessor::process(const ReceivedFrame& frame) {
  PacketProcessingResult result{};
  if (frame.length >= 4U &&
      frame.bytes[3] == static_cast<uint8_t>(protocol::MessageType::kCommandResult)) {
    result.isCommandResult = true;
    result.decodeStatus = protocol::decodeCommandResult(
        frame.bytes, frame.length, result.commandResult);
    if (result.decodeStatus != protocol::DecodeStatus::kOk) {
      ++stats_.decodeErrors;
      result.disposition = PacketDisposition::kDecodeRejected;
      return result;
    }
    ++stats_.validProtocolPackets;
    ++stats_.validCommandResultPackets;
    if (!pairing_.paired() || !latest_.available ||
        !pairing_.matches(result.commandResult.nodeId) ||
        latest_.packet.persistentSessionId !=
            result.commandResult.persistentSessionId) {
      ++stats_.commandResultsIgnored;
      result.disposition = PacketDisposition::kCommandResultIgnored;
      return result;
    }
    bool duplicate = false;
    if (!commands_.confirm(result.commandResult, duplicate)) {
      ++stats_.commandResultsIgnored;
      result.disposition = PacketDisposition::kCommandResultIgnored;
      return result;
    }
    ++stats_.commandResultsConfirmed;
    result.disposition = duplicate ? PacketDisposition::kCommandResultDuplicate
                                   : PacketDisposition::kCommandResultConfirmed;
    return result;
  }
  result.decodeStatus = protocol::decodeTelemetry(frame.bytes, frame.length,
                                                   result.telemetry);
  if (result.decodeStatus != protocol::DecodeStatus::kOk) {
    ++stats_.decodeErrors;
    result.disposition = PacketDisposition::kDecodeRejected;
    return result;
  }
  ++stats_.validProtocolPackets;
  ++stats_.validTelemetryPackets;

  if (pairing_.pairingMode()) {
    pairing_.observe(result.telemetry, frame.reception.rssiDbm,
                     frame.reception.snrDb, frame.reception.gatewayUptimeMs);
    result.disposition = PacketDisposition::kPairingCandidate;
    return result;
  }
  if (!pairing_.paired()) {
    ++stats_.unknownNodePackets;
    result.disposition = PacketDisposition::kUnpairedIgnored;
    return result;
  }
  if (!pairing_.matches(result.telemetry.nodeId)) {
    ++stats_.unknownNodePackets;
    result.disposition = PacketDisposition::kUnknownNodeIgnored;
    return result;
  }

  // The operational "latest telemetry" view is deliberately limited to the
  // explicitly paired Node. Discovery and unknown-node observations have
  // their own diagnostics and must not look like accepted production data.
  latest_.available = true;
  latest_.packet = result.telemetry;
  latest_.reception = frame.reception;

  const TelemetryKey key = makeTelemetryKey(
      result.telemetry.nodeId, result.telemetry.persistentSessionId,
      result.telemetry.sequence);
  const bool duplicate = queue_.containsKey(key);
  if (duplicate) {
    ++stats_.duplicates;
    queue_.noteDuplicate();
  } else {
    QueueRecord record{};
    record.reception = frame.reception;
    record.payloadLength = static_cast<uint8_t>(frame.length);
    memcpy(record.rawPayload, frame.bytes, frame.length);
    const uint64_t enqueueStartedUs = clock_.nowUs();
    const PacketQueueSink::EnqueueOutcome enqueue =
        queue_.durablyEnqueue(record);
    result.droppedOldest = enqueue.droppedOldest;
    if (!enqueue.persisted) {
      ++stats_.durableEnqueueFailures;
      result.disposition = PacketDisposition::kDurableEnqueueFailed;
      return result;
    }
    const uint64_t enqueueCompletedUs = clock_.nowUs();
    stats_.lastQueueWriteUs = enqueueCompletedUs - enqueueStartedUs;
    if (enqueueCompletedUs >= frame.observedUs) {
      stats_.lastRxToDurableEnqueueUs =
          enqueueCompletedUs - frame.observedUs;
    }
  }

  result.ack = ack_.transmitAck(result.telemetry);
  if (result.ack.attempted && result.ack.startUs >= frame.observedUs) {
    stats_.lastRxToAckStartUs = result.ack.startUs - frame.observedUs;
  }
  if (result.ack.attempted && result.ack.completedUs >= frame.observedUs) {
    stats_.lastRxToAckCompleteUs = result.ack.completedUs - frame.observedUs;
  }
  if (result.ack.attempted && result.ack.completedUs >= result.ack.startUs) {
    stats_.lastAckTxDurationUs =
        result.ack.completedUs - result.ack.startUs;
  }
  if (result.ack.success && result.ack.receiveRestored) {
    ++stats_.ackSent;
    if (result.ack.startUs >= frame.observedUs &&
        result.ack.completedUs >= frame.observedUs) {
      stats_.successfulAckLatency.record(
          result.ack.startUs - frame.observedUs,
          result.ack.completedUs - frame.observedUs);
    }
    result.disposition = duplicate ? PacketDisposition::kDuplicateAcknowledged
                                   : PacketDisposition::kNewAcknowledged;
  } else {
    ++stats_.ackFailures;
    result.disposition = PacketDisposition::kAckFailed;
  }
  return result;
}

}  // namespace gathra::gateway
