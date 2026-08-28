#include <unity.h>

#include <ArduinoJson.h>

#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "backend_contract.hpp"
#include "command_store.hpp"
#include "deduplicator.hpp"
#include "durable_queue_core.hpp"
#include "gateway_config.hpp"
#include "heartbeat_contract.hpp"
#include "packet_processor.hpp"
#include "pairing_manager.hpp"
#include "protocol.hpp"
#include "queue_record.hpp"
#include "firmware_version.hpp"

using namespace gathra::gateway;

namespace {

const uint8_t kNodeGolden[] = {
    0x47,0x54,0x03,0x01,0x02,0x4E,0x31,
    0x01,0x02,0x03,0x04,0xA0,0xB0,0xC0,0xD0,
    0x00,0x00,0x12,0x34,0x00,0x00,0x02,0xE4,
    0x00,0x00,0x02,0xE3,0x00,0x03,0xFB,0x2E,
    0x11,0xD7,0x0E,0x74,0x07,0x07,0x00,0x00,
    0x03,0x02,0x02,0x00,0x00,0x69,0xAB,0xCD,
    0xEF,0x0A,0x01,0x69,0xAB,0xF0,0x00,0x01,
    0x02,0x03,0x05,0x03,0x00,0x00,0x00,0x05,
    0xDC};

void pushU16(std::vector<uint8_t>& bytes, uint16_t value) {
  bytes.push_back(static_cast<uint8_t>(value >> 8U));
  bytes.push_back(static_cast<uint8_t>(value));
}
void pushU32(std::vector<uint8_t>& bytes, uint32_t value) {
  bytes.push_back(static_cast<uint8_t>(value >> 24U));
  bytes.push_back(static_cast<uint8_t>(value >> 16U));
  bytes.push_back(static_cast<uint8_t>(value >> 8U));
  bytes.push_back(static_cast<uint8_t>(value));
}

std::vector<uint8_t> packetFor(const std::string& nodeId,
                               uint32_t session = 0xF1020304U,
                               uint32_t sequence = 0xE0B0C0D0U,
                               uint8_t filter = 7U,
                               bool sentinels = false,
                               uint32_t referenceDistanceMm = 1500U) {
  std::vector<uint8_t> bytes{0x47, 0x54, 0x03, 0x01,
                             static_cast<uint8_t>(nodeId.size())};
  bytes.insert(bytes.end(), nodeId.begin(), nodeId.end());
  pushU32(bytes, session); pushU32(bytes, sequence); pushU32(bytes, 0x80000001U);
  pushU32(bytes, sentinels ? UINT32_MAX : 740U);
  pushU32(bytes, sentinels ? UINT32_MAX : 739U);
  pushU16(bytes, 3U); pushU16(bytes, sentinels ? 0x8000U : 0xFB2EU);
  pushU16(bytes, sentinels ? 0xFFFFU : 4567U); pushU16(bytes, 3700U);
  bytes.push_back(7U); bytes.push_back(7U); bytes.push_back(filter);
  pushU16(bytes, 0x001FU); pushU16(bytes, 0x0FFFU);
  bytes.push_back(static_cast<uint8_t>(protocol::BootReason::kRtcTimer));
  bytes.push_back(static_cast<uint8_t>(sentinels ? protocol::RtcState::kInvalidVl
                                                : protocol::RtcState::kValid));
  pushU32(bytes, sentinels ? 0U : 1787600000U);
  bytes.push_back(10U);
  bytes.push_back(static_cast<uint8_t>(protocol::ScheduleState::kNone));
  pushU32(bytes, 0U); pushU32(bytes, 0U);
  bytes.push_back(static_cast<uint8_t>(protocol::CommandType::kNone));
  bytes.push_back(static_cast<uint8_t>(protocol::CommandResultCode::kNone));
  pushU32(bytes, referenceDistanceMm);
  return bytes;
}

QueueRecord queueRecord(const std::vector<uint8_t>& payload,
                        uint32_t gatewayBoot = 123U) {
  QueueRecord record{};
  record.reception.gatewayReceivedUnixMs = -1;
  record.reception.gatewayUptimeMs = 456U;
  record.reception.gatewayBootSessionId = gatewayBoot;
  record.reception.rssiDbm = -91.5F;
  record.reception.snrDb = 8.25F;
  record.reception.frequencyErrorHz = -731;
  record.reception.packetLength = payload.size();
  record.payloadLength = payload.size();
  std::memcpy(record.rawPayload, payload.data(), payload.size());
  return record;
}

class MemoryStorage final : public QueueStorage {
 public:
  bool listRecordIds(std::vector<uint64_t>& ids, uint32_t& corrupt) override {
    ids.clear(); corrupt = 0;
    for (const auto& item : records) {
      if (item.first.size() != 20U || item.first.substr(16) != ".rec") {
        ++corrupt; continue;
      }
      ids.push_back(std::stoull(item.first.substr(0, 16), nullptr, 16));
    }
    return true;
  }
  bool readRecord(const std::string& name, std::vector<uint8_t>& bytes) override {
    auto found = records.find(name);
    if (found == records.end()) return false;
    bytes = found->second; return true;
  }
  bool writeRecordAtomic(uint64_t id, const std::vector<uint8_t>& bytes) override {
    if (failWrites-- > 0) return false;
    char name[32]{};
    std::snprintf(name, sizeof(name), "%016llx.rec",
                  static_cast<unsigned long long>(id));
    records[name] = bytes; return true;
  }
  bool removeRecord(const std::string& name) override {
    return records.erase(name) == 1U;
  }
  void cleanupTemporaryFiles() override { temporaryCleaned = true; }
  std::map<std::string, std::vector<uint8_t>> records;
  int failWrites = 0;
  bool temporaryCleaned = false;
};

class FakeCommandBackend final : public CommandBackend {
 public:
  bool load(CommandStoreRecord& output) override {
    if (!present) return false;
    output = record; return true;
  }
  bool save(const CommandStoreRecord& input) override {
    if (failWrites-- > 0) return false;
    record = input; present = true; return true;
  }
  CommandStoreRecord record{};
  bool present = false;
  int failWrites = 0;
};

class FakeClock final : public MonotonicClock {
 public:
  uint64_t nowUs() const override { return now += 10U; }
  mutable uint64_t now = 1000U;
};
class FakeQueue final : public PacketQueueSink {
 public:
  bool containsKey(const TelemetryKey&) override { events.push_back("contains"); return duplicate; }
  EnqueueOutcome durablyEnqueue(QueueRecord&) override { events.push_back("persist"); return {okay, false}; }
  void noteDuplicate() override { events.push_back("dedup"); }
  bool duplicate = false, okay = true;
  std::vector<std::string> events;
};
class FakeAck final : public AckSink {
 public:
  explicit FakeAck(FakeQueue& queue) : queue_(queue) {}
  AckTransmission transmitAck(const protocol::TelemetryPacket&) override {
    queue_.events.push_back("ack"); return {true, true, true, 1200U, 1300U};
  }
 private:
  FakeQueue& queue_;
};

struct ProcessorFixture {
  ProcessorFixture() : ack(queue), processor(pairing, queue, ack, commands, clock) {
    pairing.begin("N1");
    bool fresh = false;
    TEST_ASSERT_TRUE(commands.begin(commandBackend, fresh));
  }
  ReceivedFrame frame() const {
    ReceivedFrame result{};
    std::memcpy(result.bytes, kNodeGolden, sizeof(kNodeGolden));
    result.length = sizeof(kNodeGolden);
    result.observedUs = 1000U;
    result.reception.packetLength = sizeof(kNodeGolden);
    result.reception.gatewayReceivedUnixMs = -1;
    return result;
  }
  PairingManager pairing;
  FakeQueue queue;
  FakeAck ack;
  FakeCommandBackend commandBackend;
  CommandStore commands;
  FakeClock clock;
  PacketProcessor processor;
};

void test_protocol_v3_golden_and_ack_commands() {
  protocol::TelemetryPacket decoded{};
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(protocol::DecodeStatus::kOk),
      static_cast<uint8_t>(protocol::decodeTelemetry(kNodeGolden, sizeof(kNodeGolden), decoded)));
  TEST_ASSERT_EQUAL_HEX32(0x01020304U, decoded.persistentSessionId);
  TEST_ASSERT_EQUAL_UINT32(1500U, decoded.referenceDistanceMm);
  TEST_ASSERT_EQUAL_UINT(62U + std::strlen(decoded.nodeId), sizeof(kNodeGolden));
  TEST_ASSERT_EQUAL_UINT(53U, protocol::kReferenceDistancePayloadOffset);
  TEST_ASSERT_EQUAL_UINT(57U, protocol::kTelemetryPayloadBytes);
  TEST_ASSERT_EQUAL_STRING("RTC_TIMER", protocol::bootReasonName(decoded.bootReason));

  uint8_t bytes[64]{}; size_t written = 0;
  protocol::AckCommandPacket none = protocol::makeAck(decoded, 1787600000U, false);
  TEST_ASSERT_TRUE(protocol::encodeAckCommand(none, bytes, sizeof(bytes), written));
  TEST_ASSERT_EQUAL_UINT(protocol::kCommonHeaderFixedBytes +
                             std::strlen(none.nodeId) +
                             protocol::kAckCommandFixedPayloadBytes,
                         written);
  protocol::AckCommandPacket roundtrip{};
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(protocol::DecodeStatus::kOk),
      static_cast<uint8_t>(protocol::decodeAckCommand(bytes, written, roundtrip)));
  TEST_ASSERT_FALSE(roundtrip.timeValid);
  TEST_ASSERT_EQUAL_UINT32(0U, roundtrip.gatewayUnixTime);
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(protocol::CommandType::kNone),
                          static_cast<uint8_t>(roundtrip.commandType));

  protocol::AckCommandPacket ack = protocol::makeAck(decoded, 1787600000U, true);
  ack.commandId = 7U;
  ack.commandType = protocol::CommandType::kSetPollIntervalMinutes;
  ack.pollIntervalMinutes = 5U;
  TEST_ASSERT_TRUE(protocol::encodeAckCommand(ack, bytes, sizeof(bytes), written));
  TEST_ASSERT_EQUAL_UINT8(protocol::kVersion, bytes[2]);
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(protocol::DecodeStatus::kOk),
      static_cast<uint8_t>(protocol::decodeAckCommand(bytes, written, roundtrip)));
  TEST_ASSERT_TRUE(protocol::ackMatches(roundtrip, decoded));
  TEST_ASSERT_TRUE(roundtrip.timeValid);
  TEST_ASSERT_EQUAL_UINT8(5U, roundtrip.pollIntervalMinutes);
  bytes[2] = 2U;
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(protocol::DecodeStatus::kUnsupportedVersion),
      static_cast<uint8_t>(protocol::decodeAckCommand(bytes, written, roundtrip)));
  bytes[2] = protocol::kVersion;

  ack.commandType = protocol::CommandType::kScheduleMaintenanceAt;
  ack.scheduledMaintenanceUnix = 1787600400U;
  TEST_ASSERT_TRUE(protocol::encodeAckCommand(ack, bytes, sizeof(bytes), written));
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(protocol::DecodeStatus::kOk),
      static_cast<uint8_t>(protocol::decodeAckCommand(bytes, written, roundtrip)));
  ack.commandType = protocol::CommandType::kEnterMaintenanceNow;
  TEST_ASSERT_TRUE(protocol::encodeAckCommand(ack, bytes, sizeof(bytes), written));
}

void test_protocol_bounds_sentinels_and_malformed() {
  for (const std::string id : {std::string("A"), std::string(24U, 'Z')}) {
    const auto bytes = packetFor(id, UINT32_MAX, 0x80000001U, 7U, true);
    TEST_ASSERT_EQUAL_UINT(62U + id.size(), bytes.size());
    protocol::TelemetryPacket decoded{};
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(protocol::DecodeStatus::kOk),
        static_cast<uint8_t>(protocol::decodeTelemetry(bytes.data(), bytes.size(), decoded)));
    TEST_ASSERT_EQUAL_HEX32(UINT32_MAX, decoded.persistentSessionId);
    TEST_ASSERT_EQUAL_HEX32(UINT32_MAX, decoded.rawDistanceMm);
    TEST_ASSERT_EQUAL_UINT32(1500U, decoded.referenceDistanceMm);
  }
  auto bytes = packetFor("N1");
  protocol::TelemetryPacket decoded{};
  bytes[2] = 1U;
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(protocol::DecodeStatus::kUnsupportedVersion),
      static_cast<uint8_t>(protocol::decodeTelemetry(bytes.data(), bytes.size(), decoded)));
  bytes.resize(bytes.size() - 4U);
  bytes[2] = 2U;
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(protocol::DecodeStatus::kUnsupportedVersion),
      static_cast<uint8_t>(protocol::decodeTelemetry(bytes.data(), bytes.size(), decoded)));
  bytes = packetFor("N1", 1U, 2U, 7U, false, 0U);
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(protocol::DecodeStatus::kOk),
      static_cast<uint8_t>(protocol::decodeTelemetry(bytes.data(), bytes.size(), decoded)));
  TEST_ASSERT_EQUAL_UINT32(0U, decoded.referenceDistanceMm);
  bytes = packetFor("N1", 1U, 2U, 7U, false, UINT32_MAX);
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(protocol::DecodeStatus::kOk),
      static_cast<uint8_t>(protocol::decodeTelemetry(bytes.data(), bytes.size(), decoded)));
  TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, decoded.referenceDistanceMm);
  bytes = packetFor("N1"); bytes.pop_back();
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(protocol::DecodeStatus::kBufferTooSmall),
      static_cast<uint8_t>(protocol::decodeTelemetry(bytes.data(), bytes.size(), decoded)));
  bytes = packetFor("N1"); bytes.push_back(0U);
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(protocol::DecodeStatus::kTrailingData),
      static_cast<uint8_t>(protocol::decodeTelemetry(bytes.data(), bytes.size(), decoded)));
  bytes = packetFor("N!" );
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(protocol::DecodeStatus::kInvalidNodeId),
      static_cast<uint8_t>(protocol::decodeTelemetry(bytes.data(), bytes.size(), decoded)));
  bytes = packetFor("N1", 1U, 2U, 8U);
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(protocol::DecodeStatus::kInvalidEnum),
      static_cast<uint8_t>(protocol::decodeTelemetry(bytes.data(), bytes.size(), decoded)));
}

void test_command_result_codec_and_store_restart_reliability() {
  FakeCommandBackend backend;
  CommandStore store;
  bool fresh = false; uint32_t id = 0;
  TEST_ASSERT_TRUE(store.begin(backend, fresh));
  TEST_ASSERT_TRUE(store.create(protocol::CommandType::kSetPollIntervalMinutes,
                                5U, 0U, 1787600000000LL, id));
  TEST_ASSERT_EQUAL_UINT32(1U, id);
  TEST_ASSERT_TRUE(store.markSent(id, 1787600001000LL));
  CommandStore restarted;
  TEST_ASSERT_TRUE(restarted.begin(backend, fresh));
  GatewayCommand pending{};
  TEST_ASSERT_TRUE(restarted.pendingForAck(pending));
  TEST_ASSERT_EQUAL_UINT32(id, pending.commandId);
  TEST_ASSERT_EQUAL_UINT32(1U, pending.sendCount);

  protocol::CommandResultPacket result{};
  std::strcpy(result.nodeId, "N1");
  result.persistentSessionId = 0x01020304U;
  result.commandId = id;
  result.commandType = protocol::CommandType::kSetPollIntervalMinutes;
  result.resultCode = protocol::CommandResultCode::kApplied;
  result.effectivePollIntervalMinutes = 5U;
  uint8_t bytes[64]{}; size_t written = 0;
  TEST_ASSERT_TRUE(protocol::encodeCommandResult(result, bytes, sizeof(bytes), written));
  TEST_ASSERT_EQUAL_UINT8(protocol::kVersion, bytes[2]);
  TEST_ASSERT_EQUAL_UINT(protocol::kCommonHeaderFixedBytes +
                             std::strlen(result.nodeId) +
                             protocol::kCommandResultPayloadBytes,
                         written);
  protocol::CommandResultPacket decoded{};
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(protocol::DecodeStatus::kOk),
      static_cast<uint8_t>(protocol::decodeCommandResult(bytes, written, decoded)));
  bytes[2] = 2U;
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(protocol::DecodeStatus::kUnsupportedVersion),
      static_cast<uint8_t>(protocol::decodeCommandResult(bytes, written, decoded)));
  bytes[2] = protocol::kVersion;
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(protocol::DecodeStatus::kOk),
      static_cast<uint8_t>(protocol::decodeCommandResult(bytes, written, decoded)));
  bool duplicate = false;
  TEST_ASSERT_TRUE(restarted.confirm(decoded, duplicate));
  TEST_ASSERT_FALSE(duplicate);
  TEST_ASSERT_FALSE(restarted.pendingForAck(pending));
  TEST_ASSERT_TRUE(restarted.confirm(decoded, duplicate));
  TEST_ASSERT_TRUE(duplicate);
  decoded.commandId = 999U;
  TEST_ASSERT_FALSE(restarted.confirm(decoded, duplicate));
}

void test_command_allocator_does_not_reuse_after_restart() {
  FakeCommandBackend backend;
  CommandStore store;
  bool fresh = false; uint32_t first = 0, second = 0;
  TEST_ASSERT_TRUE(store.begin(backend, fresh));
  TEST_ASSERT_TRUE(store.create(protocol::CommandType::kEnterMaintenanceNow,
                                0U, 0U, -1, first));
  TEST_ASSERT_TRUE(store.cancel(first));
  CommandStore restarted;
  TEST_ASSERT_TRUE(restarted.begin(backend, fresh));
  TEST_ASSERT_TRUE(restarted.create(protocol::CommandType::kScheduleMaintenanceAt,
                                    0U, 1787600400U, 1787600000000LL, second));
  TEST_ASSERT_EQUAL_UINT32(first + 1U, second);
}

void test_packet_processor_durable_before_ack_and_command_result() {
  ProcessorFixture fixture;
  const auto processed = fixture.processor.process(fixture.frame());
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(PacketDisposition::kNewAcknowledged),
                          static_cast<uint8_t>(processed.disposition));
  TEST_ASSERT_EQUAL_STRING("contains", fixture.queue.events[0].c_str());
  TEST_ASSERT_EQUAL_STRING("persist", fixture.queue.events[1].c_str());
  TEST_ASSERT_EQUAL_STRING("ack", fixture.queue.events[2].c_str());
  const PacketProcessingStats firstStats = fixture.processor.stats();
  TEST_ASSERT_EQUAL_UINT64(1U, firstStats.ackSent);
  TEST_ASSERT_EQUAL_UINT64(1U,
      firstStats.successfulAckLatency.rxToStart.snapshot().count);
  TEST_ASSERT_EQUAL_UINT64(200U,
      firstStats.successfulAckLatency.rxToStart.snapshot().minimum);
  TEST_ASSERT_EQUAL_UINT64(300U,
      firstStats.successfulAckLatency.rxToComplete.snapshot().maximum);
  TEST_ASSERT_EQUAL_UINT64(100U,
      firstStats.successfulAckLatency.txDuration.snapshot().minimum);
  uint32_t commandId = 0;
  TEST_ASSERT_TRUE(fixture.commands.create(
      protocol::CommandType::kSetPollIntervalMinutes, 5U, 0U, -1, commandId));
  protocol::CommandResultPacket result{};
  std::strcpy(result.nodeId, "N1");
  result.persistentSessionId = 0x01020304U;
  result.commandId = commandId;
  result.commandType = protocol::CommandType::kSetPollIntervalMinutes;
  result.resultCode = protocol::CommandResultCode::kApplied;
  result.effectivePollIntervalMinutes = 5U;
  ReceivedFrame resultFrame{};
  TEST_ASSERT_TRUE(protocol::encodeCommandResult(result, resultFrame.bytes,
                                                  sizeof(resultFrame.bytes), resultFrame.length));
  const auto confirmed = fixture.processor.process(resultFrame);
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(PacketDisposition::kCommandResultConfirmed),
                          static_cast<uint8_t>(confirmed.disposition));
}

void test_pairing_dedup_queue_wrap_and_corruption() {
  PairingManager pairing;
  pairing.begin("");
  TEST_ASSERT_TRUE(pairing.start());
  protocol::TelemetryPacket telemetry{};
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(protocol::DecodeStatus::kOk),
      static_cast<uint8_t>(protocol::decodeTelemetry(kNodeGolden, sizeof(kNodeGolden), telemetry)));
  pairing.observe(telemetry, -90.0F, 8.0F, 10U);
  TEST_ASSERT_TRUE(pairing.confirmCandidate());
  TEST_ASSERT_EQUAL_HEX32(telemetry.persistentSessionId,
                          pairing.candidate().persistentSessionId);
  RecentDeduplicator dedup(2U);
  TEST_ASSERT_TRUE(dedup.remember(makeTelemetryKey("N1", 1U, 1U)));
  TEST_ASSERT_FALSE(dedup.remember(makeTelemetryKey("N1", 1U, 1U)));
  TEST_ASSERT_TRUE(dedup.remember(makeTelemetryKey("N1", 1U, 2U)));
  TEST_ASSERT_TRUE(dedup.remember(makeTelemetryKey("N1", 1U, 3U)));
  TEST_ASSERT_FALSE(dedup.contains(makeTelemetryKey("N1", 1U, 1U)));

  MemoryStorage storage;
  DurableQueueCore queue(storage, 2U);
  TEST_ASSERT_TRUE(queue.recover());
  QueueRecord one = queueRecord(packetFor("N1", 1U, 1U));
  QueueRecord two = queueRecord(packetFor("N1", 1U, 2U));
  QueueRecord three = queueRecord(packetFor("N1", 1U, 3U));
  TEST_ASSERT_TRUE(queue.enqueue(one)); TEST_ASSERT_TRUE(queue.enqueue(two));
  TEST_ASSERT_TRUE(queue.enqueue(three));
  TEST_ASSERT_EQUAL_UINT32(1U, queue.stats().droppedOldest);
  storage.records["ffffffffffffffff.rec"] = {0x47, 0x54, 0x03};
  DurableQueueCore recovered(storage, 2U);
  TEST_ASSERT_TRUE(recovered.recover());
  TEST_ASSERT_EQUAL_UINT32(1U, recovered.stats().corruptRecords);

  const auto maximumPacket = packetFor(std::string(24U, 'Z'), UINT32_MAX,
                                       UINT32_MAX, 7U, false, UINT32_MAX);
  TEST_ASSERT_EQUAL_UINT(protocol::kMaximumTelemetryPacketBytes,
                         maximumPacket.size());
  QueueRecord maximumRecord = queueRecord(maximumPacket);
  maximumRecord.recordId = 9U;
  std::vector<uint8_t> encodedRecord;
  TEST_ASSERT_TRUE(QueueRecordCodec::encode(maximumRecord, encodedRecord));
  TEST_ASSERT_EQUAL_UINT(QueueRecordCodec::kFixedBytesWithoutPayload +
                             protocol::kMaximumTelemetryPacketBytes,
                         encodedRecord.size());
  QueueRecord recoveredMaximum{};
  TEST_ASSERT_TRUE(QueueRecordCodec::decode(encodedRecord.data(),
                                             encodedRecord.size(),
                                             recoveredMaximum));
  TEST_ASSERT_EQUAL_UINT8_ARRAY(maximumPacket.data(), recoveredMaximum.rawPayload,
                                maximumPacket.size());
}

void test_configuration_and_backend_payload() {
  GatewayConfig config;
  config.setDefaults("GTH-GW-AABBCCDDEEFF");
  TEST_ASSERT_TRUE(validateConfig(config));
  TEST_ASSERT_EQUAL_UINT32(60U, config.heartbeatIntervalSeconds);
  config.heartbeatIntervalSeconds = 14U;
  TEST_ASSERT_FALSE(validateConfig(config));
  config.heartbeatIntervalSeconds = 3601U;
  TEST_ASSERT_FALSE(validateConfig(config));
  config.heartbeatIntervalSeconds = 15U;
  TEST_ASSERT_TRUE(validateConfig(config));
  config.radio.spreadingFactor = 13U;
  TEST_ASSERT_FALSE(validateConfig(config));
  TEST_ASSERT_FALSE(protocol::nodeIdValid("bad id"));

  GatewayIdentity identity{};
  std::strcpy(identity.gatewayId, "GTH-GW-AABBCCDDEEFF");
  std::strcpy(identity.hardwareMac, "AA:BB:CC:DD:EE:FF");
  std::strcpy(identity.firmwareVersion, "2.2.0");
  identity.bootSessionId = 123U;
  QueueRecord record = queueRecord(std::vector<uint8_t>(std::begin(kNodeGolden),
                                                        std::end(kNodeGolden)));
  record.recordId = 1U;
  std::string json;
  TEST_ASSERT_TRUE(serializeBackendBatch(identity, {record}, json));
  JsonDocument parsed;
  TEST_ASSERT_FALSE(static_cast<bool>(deserializeJson(parsed, json)));
  TEST_ASSERT_EQUAL_STRING("2.2.0",
      parsed["gateway"]["firmwareVersion"].as<const char*>());
  TEST_ASSERT_EQUAL_UINT16(sizeof(kNodeGolden),
      parsed["readings"][0]["packetLength"].as<uint16_t>());
  TEST_ASSERT_EQUAL_STRING(
      "R1QDAQJOMQECAwSgsMDQAAASNAAAAuQAAALjAAP7LhHXDnQHBwAAAwICAABpq83vCgFpq/AAAQIDBQMAAAAF3A==",
      parsed["readings"][0]["rawPayloadBase64"].as<const char*>());
  TEST_ASSERT_FALSE(parsed["readings"][0]["gatewayTimeTrusted"].as<bool>());

  std::vector<BackendRecordResult> results;
  TEST_ASSERT_TRUE(parseBackendBatchResponse(
      "{\"results\":[{\"index\":0,\"status\":\"INSERTED\"}]}", 1U, results));
  TEST_ASSERT_TRUE(backendBatchResponseComplete(1U, results));
  TEST_ASSERT_FALSE(backendHttpResponseMayDequeue(503));
}

void test_configuration_v1_migration_preserves_operator_values() {
  GatewayConfigV1 legacy{};
  std::strcpy(legacy.gatewayId, "GTH-GW-MIGRATION");
  std::strcpy(legacy.wifiSsid, "Field WiFi");
  std::strcpy(legacy.wifiPassword, "password123");
  std::strcpy(legacy.pairedNodeId, "NODE_1");
  std::strcpy(legacy.backendBaseUrl, "https://api.gathra.my.id");
  std::strcpy(legacy.backendBearerToken, "preserved-token");
  legacy.radio.frequencyMhz = 434.0F;
  legacy.backendBatchSize = 17U;
  legacy.backendHttpTimeoutMs = 7000U;
  GatewayConfig migrated{};
  TEST_ASSERT_TRUE(migrateConfigV1(legacy, migrated));
  TEST_ASSERT_TRUE(validateConfig(migrated));
  TEST_ASSERT_EQUAL_UINT16(build::kConfigSchemaVersion,
                           migrated.schemaVersion);
  TEST_ASSERT_EQUAL_STRING(legacy.gatewayId, migrated.gatewayId);
  TEST_ASSERT_EQUAL_STRING(legacy.wifiSsid, migrated.wifiSsid);
  TEST_ASSERT_EQUAL_STRING(legacy.wifiPassword, migrated.wifiPassword);
  TEST_ASSERT_EQUAL_STRING(legacy.pairedNodeId, migrated.pairedNodeId);
  TEST_ASSERT_EQUAL_STRING(legacy.backendBearerToken,
                           migrated.backendBearerToken);
  TEST_ASSERT_EQUAL_FLOAT(434.0F, migrated.radio.frequencyMhz);
  TEST_ASSERT_EQUAL_UINT8(17U, migrated.backendBatchSize);
  TEST_ASSERT_EQUAL_UINT32(7000U, migrated.backendHttpTimeoutMs);
  TEST_ASSERT_EQUAL_UINT32(build::kDefaultHeartbeatIntervalSeconds,
                           migrated.heartbeatIntervalSeconds);
  legacy.schemaVersion = 2U;
  TEST_ASSERT_FALSE(migrateConfigV1(legacy, migrated));
}

void test_heartbeat_scheduler_bounds_and_queue_priority_model() {
  HeartbeatScheduler scheduler;
  TEST_ASSERT_FALSE(scheduler.configure(14U, 1'000U));
  TEST_ASSERT_FALSE(scheduler.configure(3601U, 1'000U));
  TEST_ASSERT_TRUE(scheduler.configure(60U, 1'000U));
  TEST_ASSERT_FALSE(scheduler.due(60'000'999U));
  TEST_ASSERT_TRUE(scheduler.due(60'001'000U));
  // A pending durable record prevents the worker from marking an attempt;
  // therefore the due heartbeat remains eligible after telemetry drains.
  const bool durableTelemetryPending = true;
  if (!durableTelemetryPending) scheduler.markAttempt(60'001'000U);
  TEST_ASSERT_TRUE(scheduler.due(60'001'000U));
  scheduler.markAttempt(60'001'000U);
  TEST_ASSERT_FALSE(scheduler.due(120'000'999U));
  TEST_ASSERT_TRUE(scheduler.due(120'001'000U));
  TEST_ASSERT_TRUE(heartbeatIntervalValid(15U));
  TEST_ASSERT_TRUE(heartbeatIntervalValid(3600U));
}

HeartbeatSnapshot populatedHeartbeat() {
  HeartbeatSnapshot snapshot{};
  std::strcpy(snapshot.gateway.gatewayId, "GTH-GW-AABBCCDDEEFF");
  std::strcpy(snapshot.gateway.mac, "AA:BB:CC:DD:EE:FF");
  std::strcpy(snapshot.gateway.firmwareVersion, firmware::kVersion);
  snapshot.gateway.protocolVersion = protocol::kVersion;
  std::strcpy(snapshot.gateway.buildFlavor, "production");
  snapshot.runtime.uptimeSeconds = 12345U;
  std::strcpy(snapshot.runtime.resetReason, "POWER_ON");
  snapshot.runtime.bootCount = 12U;
  snapshot.runtime.freeHeapBytes = 123456U;
  snapshot.runtime.minFreeHeapBytes = 100000U;
  snapshot.runtime.largestFreeHeapBlockBytes = 80000U;
  snapshot.runtime.sketchSizeBytes = 1200000U;
  snapshot.runtime.freeSketchSpaceBytes = 200000U;
  snapshot.runtime.flashSizeBytes = 4194304U;
  snapshot.network.wifiConnected = true;
  std::strcpy(snapshot.network.ssid, "Lab \"A\"");
  snapshot.network.wifiRssiDbm = -55;
  std::strcpy(snapshot.network.localIp, "192.168.1.20");
  snapshot.network.backendConnectivityState =
      BackendConnectivityState::kHealthy;
  snapshot.network.lastBackendSuccessUnixMs = 1787600000000LL;
  snapshot.time.valid = true;
  snapshot.time.currentUnixMs = 1787600123000LL;
  snapshot.time.lastNtpSyncUnixMs = 1787600000000LL;
  snapshot.lora.paired = true;
  std::strcpy(snapshot.lora.pairedNodeId, "N1");
  snapshot.lora.latestReceptionAvailable = true;
  snapshot.lora.lastRxUnixMs = 1787600100000LL;
  snapshot.lora.latestRssiDbm = -45.5F;
  snapshot.lora.latestSnrDb = 10.25F;
  snapshot.lora.latestFrequencyErrorHz = 1050;
  snapshot.lora.receivedPacketCount = 20U;
  snapshot.lora.validTelemetryCount = 18U;
  snapshot.ack.successCount = 2U;
  snapshot.ack.failureCount = 1U;
  snapshot.ack.count = 3U;
  snapshot.ack.latestAvailable = true;
  snapshot.ack.latestRxToStartUs = 30'200U;
  snapshot.ack.latestRxToCompleteUs = 642'100U;
  AckLatencyStatistics latency;
  latency.record(30'000U, 640'000U);
  latency.record(40'000U, 660'000U);
  snapshot.ack.rxToStart = latency.rxToStart.snapshot();
  snapshot.ack.rxToComplete = latency.rxToComplete.snapshot();
  snapshot.ack.txDuration = latency.txDuration.snapshot();
  snapshot.queue.depth = 3U;
  snapshot.queue.capacity = 128U;
  snapshot.queue.oldestAgeAvailable = true;
  snapshot.queue.oldestRecordAgeSeconds = 90U;
  snapshot.queue.telemetryUploadSuccessCount = 7U;
  snapshot.queue.telemetryUploadFailureCount = 2U;
  snapshot.commands.pending = true;
  snapshot.commands.pendingCommandId = 44U;
  std::strcpy(snapshot.commands.pendingCommandType,
              "SET_POLL_INTERVAL_MINUTES");
  std::strcpy(snapshot.commands.pendingCommandState, "SENT");
  snapshot.commands.lastAvailable = true;
  snapshot.commands.lastCommandId = 44U;
  std::strcpy(snapshot.commands.lastCommandResult, "NONE");
  snapshot.commands.commandsSentCount = 5U;
  snapshot.commands.commandResultsReceivedCount = 4U;
  return snapshot;
}

void test_heartbeat_json_schema_escaping_metrics_and_nulls() {
  HeartbeatSnapshot snapshot = populatedHeartbeat();
  MemoryStorage storage;
  DurableQueueCore durableQueue(storage, 8U);
  TEST_ASSERT_TRUE(durableQueue.recover());
  TEST_ASSERT_EQUAL_UINT(0U, durableQueue.size());
  std::string json;
  TEST_ASSERT_TRUE(serializeHeartbeat(snapshot, json));
  // Serialization has no queue sink and cannot consume durable capacity.
  TEST_ASSERT_EQUAL_UINT(0U, durableQueue.size());
  JsonDocument parsed;
  TEST_ASSERT_FALSE(static_cast<bool>(deserializeJson(parsed, json)));
  TEST_ASSERT_EQUAL_UINT8(1U, parsed["schemaVersion"].as<uint8_t>());
  TEST_ASSERT_EQUAL_UINT32(60U,
      parsed["heartbeatIntervalSeconds"].as<uint32_t>());
  TEST_ASSERT_EQUAL_STRING("2.2.0",
      parsed["gateway"]["firmwareVersion"].as<const char*>());
  TEST_ASSERT_EQUAL_UINT8(3U,
      parsed["gateway"]["protocolVersion"].as<uint8_t>());
  TEST_ASSERT_EQUAL_STRING("Lab \"A\"",
      parsed["network"]["ssid"].as<const char*>());
  TEST_ASSERT_EQUAL_STRING("HEALTHY",
      parsed["network"]["backendConnectivityState"].as<const char*>());
  TEST_ASSERT_EQUAL_UINT64(123U,
      parsed["time"]["ntpAgeSeconds"].as<uint64_t>());
  TEST_ASSERT_FLOAT_WITHIN(0.001F, 30.2F,
      parsed["ack"]["latestRxToAckStartMs"].as<float>());
  TEST_ASSERT_FLOAT_WITHIN(0.001F, 611.9F,
      parsed["ack"]["latestAckTxDurationMs"].as<float>());
  TEST_ASSERT_FLOAT_WITHIN(0.001F, 35.0F,
      parsed["ack"]["avgRxToAckStartMs"].as<float>());
  TEST_ASSERT_EQUAL_UINT32(3U, parsed["queue"]["depth"].as<uint32_t>());
  TEST_ASSERT_EQUAL_UINT64(2U,
      parsed["queue"]["telemetryUploadFailureCount"].as<uint64_t>());

  snapshot.time.valid = false;
  snapshot.network.wifiConnected = false;
  snapshot.lora.latestReceptionAvailable = false;
  snapshot.lora.lastRxUnixMs = -1;
  snapshot.ack = {};
  snapshot.queue.oldestAgeAvailable = false;
  TEST_ASSERT_TRUE(serializeHeartbeat(snapshot, json));
  TEST_ASSERT_FALSE(static_cast<bool>(deserializeJson(parsed, json)));
  TEST_ASSERT_TRUE(parsed["time"]["currentUtc"].isNull());
  TEST_ASSERT_TRUE(parsed["time"]["lastNtpSyncAt"].isNull());
  TEST_ASSERT_TRUE(parsed["network"]["wifiRssiDbm"].isNull());
  TEST_ASSERT_TRUE(parsed["network"]["localIp"].isNull());
  TEST_ASSERT_TRUE(parsed["lora"]["latestRssiDbm"].isNull());
  TEST_ASSERT_TRUE(parsed["ack"]["latestRxToAckStartMs"].isNull());
  TEST_ASSERT_TRUE(parsed["ack"]["avgRxToAckStartMs"].isNull());
  TEST_ASSERT_TRUE(parsed["queue"]["oldestRecordAgeSeconds"].isNull());
}

void test_heartbeat_http_failures_and_connectivity_classification() {
  TEST_ASSERT_TRUE(heartbeatHttpSucceeded(200));
  TEST_ASSERT_TRUE(heartbeatHttpSucceeded(202));
  for (const int status : {0, 401, 404, 500, 503}) {
    TEST_ASSERT_FALSE(heartbeatHttpSucceeded(status));
  }
  TEST_ASSERT_EQUAL_UINT8(
      static_cast<uint8_t>(BackendConnectivityState::kUnknown),
      static_cast<uint8_t>(classifyBackendConnectivity(0U, 0U)));
  TEST_ASSERT_EQUAL_UINT8(
      static_cast<uint8_t>(BackendConnectivityState::kHealthy),
      static_cast<uint8_t>(classifyBackendConnectivity(1U, 0U)));
  TEST_ASSERT_EQUAL_UINT8(
      static_cast<uint8_t>(BackendConnectivityState::kDegraded),
      static_cast<uint8_t>(classifyBackendConnectivity(1U, 2U)));
  TEST_ASSERT_EQUAL_UINT8(
      static_cast<uint8_t>(BackendConnectivityState::kOffline),
      static_cast<uint8_t>(classifyBackendConnectivity(1U, 3U)));
  TEST_ASSERT_EQUAL_UINT8(3U, protocol::kVersion);
  TEST_ASSERT_EQUAL_STRING("2.2.0", firmware::kVersion);
}

}  // namespace

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_protocol_v3_golden_and_ack_commands);
  RUN_TEST(test_protocol_bounds_sentinels_and_malformed);
  RUN_TEST(test_command_result_codec_and_store_restart_reliability);
  RUN_TEST(test_command_allocator_does_not_reuse_after_restart);
  RUN_TEST(test_packet_processor_durable_before_ack_and_command_result);
  RUN_TEST(test_pairing_dedup_queue_wrap_and_corruption);
  RUN_TEST(test_configuration_and_backend_payload);
  RUN_TEST(test_configuration_v1_migration_preserves_operator_values);
  RUN_TEST(test_heartbeat_scheduler_bounds_and_queue_priority_model);
  RUN_TEST(test_heartbeat_json_schema_escaping_metrics_and_nulls);
  RUN_TEST(test_heartbeat_http_failures_and_connectivity_classification);
  return UNITY_END();
}
