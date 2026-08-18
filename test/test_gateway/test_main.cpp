#include <unity.h>

#include <ArduinoJson.h>

#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "backend_contract.hpp"
#include "deduplicator.hpp"
#include "durable_queue_core.hpp"
#include "gateway_config.hpp"
#include "packet_processor.hpp"
#include "pairing_manager.hpp"
#include "protocol.hpp"
#include "queue_record.hpp"

using namespace gathra::gateway;

namespace {

const uint8_t kNodeGolden[] = {
    0x47, 0x54, 0x01, 0x01, 0x02, 0x4E, 0x31,
    0x01, 0x02, 0x03, 0x04, 0xA0, 0xB0, 0xC0, 0xD0,
    0x00, 0x00, 0x12, 0x34, 0x00, 0x00, 0x02, 0xE4,
    0x00, 0x00, 0x02, 0xE3, 0x00, 0x03, 0xFB, 0x2E,
    0x11, 0xD7, 0x0E, 0x74, 0x07, 0x07, 0x00, 0x00,
    0x03, 0x02, 0x02};

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
                               uint32_t boot = 0xF1020304U,
                               uint32_t sequence = 0xE0B0C0D0U,
                               uint8_t filter = 7U,
                               bool sentinels = false) {
  std::vector<uint8_t> bytes{0x47, 0x54, 0x01, 0x01,
                             static_cast<uint8_t>(nodeId.size())};
  bytes.insert(bytes.end(), nodeId.begin(), nodeId.end());
  pushU32(bytes, boot);
  pushU32(bytes, sequence);
  pushU32(bytes, 0x80000001U);
  pushU32(bytes, sentinels ? UINT32_MAX : 740U);
  pushU32(bytes, sentinels ? UINT32_MAX : 739U);
  pushU16(bytes, 3U);
  pushU16(bytes, sentinels ? 0x8000U : 0xFB2EU);
  pushU16(bytes, sentinels ? 0xFFFFU : 4567U);
  pushU16(bytes, 3700U);
  bytes.push_back(7U);
  bytes.push_back(7U);
  bytes.push_back(filter);
  pushU16(bytes, 0x001FU);
  pushU16(bytes, 0x0FFFU);
  return bytes;
}

QueueRecord queueRecord(const std::vector<uint8_t>& payload,
                        uint32_t boot = 123U) {
  QueueRecord record{};
  record.reception.gatewayReceivedUnixMs = -1;
  record.reception.gatewayTimeTrusted = false;
  record.reception.gatewayUptimeMs = 456U;
  record.reception.gatewayBootSessionId = boot;
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
  bool listRecordIds(std::vector<uint64_t>& recordIds,
                     uint32_t& corruptNames) override {
    recordIds.clear();
    corruptNames = 0;
    for (const auto& item : records) {
      if (item.first.size() != 20U || item.first.substr(16) != ".rec") {
        ++corruptNames;
        continue;
      }
      recordIds.push_back(std::stoull(item.first.substr(0, 16), nullptr, 16));
    }
    return listOkay;
  }
  bool readRecord(const std::string& name, std::vector<uint8_t>& bytes) override {
    const auto found = records.find(name);
    if (found == records.end()) return false;
    bytes = found->second;
    return true;
  }
  bool writeRecordAtomic(uint64_t id, const std::vector<uint8_t>& bytes) override {
    if (failWrites > 0) {
      --failWrites;
      return false;
    }
    char name[32]{};
    std::snprintf(name, sizeof(name), "%016llx.rec",
                  static_cast<unsigned long long>(id));
    records[name] = bytes;
    return true;
  }
  bool removeRecord(const std::string& name) override {
    if (failRemove) return false;
    return records.erase(name) == 1U;
  }
  void cleanupTemporaryFiles() override { temporaryCleaned = true; }

  std::map<std::string, std::vector<uint8_t>> records;
  int failWrites = 0;
  bool failRemove = false;
  bool listOkay = true;
  bool temporaryCleaned = false;
};

class FakeClock final : public MonotonicClock {
 public:
  uint64_t nowUs() const override { return now += 10U; }
  mutable uint64_t now = 1000U;
};

class FakeQueue final : public PacketQueueSink {
 public:
  bool containsKey(const TelemetryKey&) override {
    events.push_back("contains");
    return duplicate;
  }
  EnqueueOutcome durablyEnqueue(QueueRecord&) override {
    events.push_back("persist");
    return {enqueueOkay, dropOldest};
  }
  void noteDuplicate() override { events.push_back("dedup"); }
  bool duplicate = false;
  bool enqueueOkay = true;
  bool dropOldest = false;
  std::vector<std::string> events;
};

class FakeAck final : public AckSink {
 public:
  explicit FakeAck(FakeQueue& queue) : queue(queue) {}
  AckTransmission transmitAck(const protocol::TelemetryPacket&) override {
    queue.events.push_back("ack");
    return {true, okay, true, 1200U, 1300U};
  }
  FakeQueue& queue;
  bool okay = true;
};

void test_protocol_matches_node_golden_and_ack() {
  protocol::TelemetryPacket decoded{};
  TEST_ASSERT_EQUAL_UINT8(
      static_cast<uint8_t>(protocol::DecodeStatus::kOk),
      static_cast<uint8_t>(protocol::decodeTelemetry(kNodeGolden,
                                                     sizeof(kNodeGolden), decoded)));
  TEST_ASSERT_EQUAL_STRING("N1", decoded.nodeId);
  TEST_ASSERT_EQUAL_HEX32(0x01020304U, decoded.bootSessionId);
  TEST_ASSERT_EQUAL_HEX32(0xA0B0C0D0U, decoded.sequence);
  TEST_ASSERT_EQUAL_INT16(-1234, decoded.temperatureCentiC);
  TEST_ASSERT_EQUAL_UINT16(0x0202U, decoded.healthFlags);

  const uint8_t expectedAck[] = {
      0x47, 0x54, 0x01, 0x02, 0x02, 0x4E, 0x31,
      0x01, 0x02, 0x03, 0x04, 0xA0, 0xB0, 0xC0, 0xD0};
  uint8_t ackBytes[32]{};
  size_t written = 0;
  TEST_ASSERT_TRUE(protocol::encodeAck(protocol::makeAck(decoded), ackBytes,
                                       sizeof(ackBytes), written));
  TEST_ASSERT_EQUAL_UINT(sizeof(expectedAck), written);
  TEST_ASSERT_EQUAL_UINT8_ARRAY(expectedAck, ackBytes, sizeof(expectedAck));
  protocol::AckPacket ack{};
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(protocol::DecodeStatus::kOk),
                          static_cast<uint8_t>(protocol::decodeAck(ackBytes, written, ack)));
  TEST_ASSERT_TRUE(protocol::ackMatches(ack, decoded));
}

void test_protocol_node_id_bounds_sentinels_and_uint32() {
  for (const std::string nodeId : {std::string("A"), std::string(24U, 'Z')}) {
    const auto bytes = packetFor(nodeId, 0xFFFFFFFFU, 0x80000001U, 7U, true);
    TEST_ASSERT_EQUAL_UINT(40U + nodeId.size(), bytes.size());
    protocol::TelemetryPacket decoded{};
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(protocol::DecodeStatus::kOk),
                            static_cast<uint8_t>(protocol::decodeTelemetry(
                                bytes.data(), bytes.size(), decoded)));
    TEST_ASSERT_EQUAL_HEX32(0xFFFFFFFFU, decoded.bootSessionId);
    TEST_ASSERT_EQUAL_HEX32(0x80000001U, decoded.sequence);
    TEST_ASSERT_EQUAL_HEX32(UINT32_MAX, decoded.rawDistanceMm);
    TEST_ASSERT_EQUAL_INT16(INT16_MIN, decoded.temperatureCentiC);
    TEST_ASSERT_EQUAL_UINT16(UINT16_MAX, decoded.humidityCentiPercent);
  }
}

void test_protocol_rejects_all_malformed_framing() {
  auto bytes = packetFor("N1");
  protocol::TelemetryPacket decoded{};
  bytes[0] = 0;
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(protocol::DecodeStatus::kBadMagic),
                          static_cast<uint8_t>(protocol::decodeTelemetry(bytes.data(), bytes.size(), decoded)));
  bytes = packetFor("N1"); bytes[2] = 2U;
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(protocol::DecodeStatus::kUnsupportedVersion),
                          static_cast<uint8_t>(protocol::decodeTelemetry(bytes.data(), bytes.size(), decoded)));
  bytes = packetFor("N1"); bytes[3] = 2U;
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(protocol::DecodeStatus::kWrongType),
                          static_cast<uint8_t>(protocol::decodeTelemetry(bytes.data(), bytes.size(), decoded)));
  bytes = packetFor("N1");
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(protocol::DecodeStatus::kBufferTooSmall),
                          static_cast<uint8_t>(protocol::decodeTelemetry(bytes.data(), bytes.size() - 1U, decoded)));
  bytes.push_back(0U);
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(protocol::DecodeStatus::kTrailingData),
                          static_cast<uint8_t>(protocol::decodeTelemetry(bytes.data(), bytes.size(), decoded)));
  bytes = packetFor("N!");
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(protocol::DecodeStatus::kInvalidNodeId),
                          static_cast<uint8_t>(protocol::decodeTelemetry(bytes.data(), bytes.size(), decoded)));
  bytes = packetFor("ABC"); bytes[6] = 0U;
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(protocol::DecodeStatus::kInvalidNodeId),
                          static_cast<uint8_t>(protocol::decodeTelemetry(bytes.data(), bytes.size(), decoded)));
  bytes = packetFor("ABC"); bytes[6] = 0xE9U;
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(protocol::DecodeStatus::kInvalidNodeId),
                          static_cast<uint8_t>(protocol::decodeTelemetry(bytes.data(), bytes.size(), decoded)));
  bytes = packetFor("N1", 1U, 2U, 8U);
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(protocol::DecodeStatus::kInvalidFilterState),
                          static_cast<uint8_t>(protocol::decodeTelemetry(bytes.data(), bytes.size(), decoded)));
}

void test_pairing_is_explicit_and_single_node() {
  PairingManager pairing;
  pairing.begin("");
  TEST_ASSERT_FALSE(pairing.paired());
  TEST_ASSERT_TRUE(pairing.start());
  protocol::TelemetryPacket telemetry{};
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(protocol::DecodeStatus::kOk),
                          static_cast<uint8_t>(protocol::decodeTelemetry(kNodeGolden, sizeof(kNodeGolden), telemetry)));
  pairing.observe(telemetry, -91.5F, 8.25F, 99U);
  TEST_ASSERT_TRUE(pairing.candidate().available);
  TEST_ASSERT_FALSE(pairing.paired());
  TEST_ASSERT_TRUE(pairing.confirmCandidate());
  TEST_ASSERT_TRUE(pairing.matches("N1"));
  TEST_ASSERT_FALSE(pairing.matches("N2"));
  TEST_ASSERT_FALSE(pairing.start());
  pairing.unpair();
  TEST_ASSERT_TRUE(pairing.pairManual("N2"));
  TEST_ASSERT_TRUE(pairing.matches("N2"));
  TEST_ASSERT_FALSE(pairing.pairManual("bad id"));
}

void test_dedup_exact_tuple_and_boot_sessions() {
  RecentDeduplicator dedup(3);
  const TelemetryKey first = makeTelemetryKey("N1", 10U, 1U);
  TEST_ASSERT_TRUE(dedup.remember(first));
  TEST_ASSERT_FALSE(dedup.remember(first));
  TEST_ASSERT_FALSE(dedup.contains(makeTelemetryKey("N1", 11U, 1U)));
  TEST_ASSERT_TRUE(dedup.remember(makeTelemetryKey("N1", 11U, 1U)));
  TEST_ASSERT_TRUE(dedup.remember(makeTelemetryKey("N1", 11U, 2U)));
  TEST_ASSERT_TRUE(dedup.remember(makeTelemetryKey("N1", 11U, 3U)));
  TEST_ASSERT_FALSE(dedup.contains(first));
}

void test_queue_codec_core_recovery_overflow_and_corruption() {
  MemoryStorage storage;
  DurableQueueCore queue(storage, 2U);
  TEST_ASSERT_TRUE(queue.recover());
  TEST_ASSERT_TRUE(storage.temporaryCleaned);
  TEST_ASSERT_EQUAL_UINT(0U, queue.size());
  QueueRecord one = queueRecord(packetFor("N1", 1U, 1U));
  QueueRecord two = queueRecord(packetFor("N1", 1U, 2U));
  QueueRecord three = queueRecord(packetFor("N1", 1U, 3U));
  TEST_ASSERT_TRUE(queue.enqueue(one));
  TEST_ASSERT_TRUE(queue.enqueue(two));
  TEST_ASSERT_TRUE(queue.contains(makeTelemetryKey("N1", 1U, 1U)));
  TEST_ASSERT_TRUE(queue.enqueue(three));
  TEST_ASSERT_EQUAL_UINT(2U, queue.size());
  TEST_ASSERT_EQUAL_UINT32(1U, queue.stats().droppedOldest);
  TEST_ASSERT_FALSE(queue.contains(makeTelemetryKey("N1", 1U, 1U)));

  std::vector<QueueRecord> records;
  TEST_ASSERT_TRUE(queue.peek(10U, records));
  TEST_ASSERT_EQUAL_UINT(2U, records.size());
  TEST_ASSERT_EQUAL_UINT64(two.recordId, records[0].recordId);
  TEST_ASSERT_TRUE(queue.remove(two.recordId));

  // A truncated/corrupt committed record is detected and removed during boot recovery.
  storage.records["ffffffffffffffff.rec"] = {0x47, 0x54, 0x01};
  DurableQueueCore recovered(storage, 2U);
  TEST_ASSERT_TRUE(recovered.recover());
  TEST_ASSERT_EQUAL_UINT32(1U, recovered.stats().corruptRecords);
  TEST_ASSERT_EQUAL_UINT(1U, recovered.size());
  TEST_ASSERT_TRUE(recovered.contains(makeTelemetryKey("N1", 1U, 3U)));
}

void test_queue_full_write_failure_is_explicit() {
  MemoryStorage storage;
  DurableQueueCore queue(storage, 1U);
  TEST_ASSERT_TRUE(queue.recover());
  QueueRecord one = queueRecord(packetFor("N1", 1U, 1U));
  QueueRecord two = queueRecord(packetFor("N1", 1U, 2U));
  TEST_ASSERT_TRUE(queue.enqueue(one));
  storage.failWrites = 2;
  TEST_ASSERT_FALSE(queue.enqueue(two));
  TEST_ASSERT_EQUAL_UINT32(1U, queue.stats().droppedOldest);
  TEST_ASSERT_EQUAL_UINT32(1U, queue.stats().storageErrors);
}

void test_packet_processor_enforces_durable_before_ack_and_pairing() {
  PairingManager pairing;
  pairing.begin("N1");
  FakeQueue queue;
  FakeAck ack(queue);
  FakeClock clock;
  PacketProcessor processor(pairing, queue, ack, clock);
  ReceivedFrame frame{};
  std::memcpy(frame.bytes, kNodeGolden, sizeof(kNodeGolden));
  frame.length = sizeof(kNodeGolden);
  frame.observedUs = 1000U;
  frame.reception.packetLength = sizeof(kNodeGolden);
  frame.reception.gatewayReceivedUnixMs = -1;
  const auto result = processor.process(frame);
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(PacketDisposition::kNewAcknowledged),
                          static_cast<uint8_t>(result.disposition));
  TEST_ASSERT_EQUAL_STRING("contains", queue.events[0].c_str());
  TEST_ASSERT_EQUAL_STRING("persist", queue.events[1].c_str());
  TEST_ASSERT_EQUAL_STRING("ack", queue.events[2].c_str());
  TEST_ASSERT_FALSE(result.droppedOldest);
  TEST_ASSERT_EQUAL_UINT64(10U, processor.stats().lastQueueWriteUs);
  TEST_ASSERT_EQUAL_UINT64(20U,
                           processor.stats().lastRxToDurableEnqueueUs);

  queue.events.clear();
  queue.enqueueOkay = false;
  queue.dropOldest = true;
  const auto failed = processor.process(frame);
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(PacketDisposition::kDurableEnqueueFailed),
                          static_cast<uint8_t>(failed.disposition));
  TEST_ASSERT_EQUAL_UINT(2U, queue.events.size());
  TEST_ASSERT_EQUAL_STRING("persist", queue.events[1].c_str());
  TEST_ASSERT_TRUE(failed.droppedOldest);
}

void test_packet_processor_duplicate_reacks_without_enqueue() {
  PairingManager pairing;
  pairing.begin("N1");
  FakeQueue queue;
  queue.duplicate = true;
  FakeAck ack(queue);
  FakeClock clock;
  PacketProcessor processor(pairing, queue, ack, clock);
  ReceivedFrame frame{};
  std::memcpy(frame.bytes, kNodeGolden, sizeof(kNodeGolden));
  frame.length = sizeof(kNodeGolden);
  frame.observedUs = 1000U;
  frame.reception.packetLength = sizeof(kNodeGolden);
  frame.reception.gatewayReceivedUnixMs = -1;
  const auto result = processor.process(frame);
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(PacketDisposition::kDuplicateAcknowledged),
                          static_cast<uint8_t>(result.disposition));
  TEST_ASSERT_EQUAL_UINT(3U, queue.events.size());
  TEST_ASSERT_EQUAL_STRING("contains", queue.events[0].c_str());
  TEST_ASSERT_EQUAL_STRING("dedup", queue.events[1].c_str());
  TEST_ASSERT_EQUAL_STRING("ack", queue.events[2].c_str());
}

void test_packet_processor_keeps_discovery_and_unknown_nodes_out_of_production() {
  PairingManager pairing;
  pairing.begin("");
  FakeQueue queue;
  FakeAck ack(queue);
  FakeClock clock;
  PacketProcessor processor(pairing, queue, ack, clock);
  ReceivedFrame frame{};
  std::memcpy(frame.bytes, kNodeGolden, sizeof(kNodeGolden));
  frame.length = sizeof(kNodeGolden);
  frame.observedUs = 1000U;
  frame.reception.packetLength = sizeof(kNodeGolden);
  frame.reception.gatewayReceivedUnixMs = -1;

  const auto unpaired = processor.process(frame);
  TEST_ASSERT_EQUAL_UINT8(
      static_cast<uint8_t>(PacketDisposition::kUnpairedIgnored),
      static_cast<uint8_t>(unpaired.disposition));
  TEST_ASSERT_TRUE(queue.events.empty());
  TEST_ASSERT_FALSE(processor.latest().available);

  TEST_ASSERT_TRUE(pairing.start());
  const auto discovery = processor.process(frame);
  TEST_ASSERT_EQUAL_UINT8(
      static_cast<uint8_t>(PacketDisposition::kPairingCandidate),
      static_cast<uint8_t>(discovery.disposition));
  TEST_ASSERT_TRUE(pairing.candidate().available);
  TEST_ASSERT_TRUE(queue.events.empty());
  TEST_ASSERT_FALSE(processor.latest().available);
  TEST_ASSERT_TRUE(pairing.confirmCandidate());

  const auto unknownBytes = packetFor("N2");
  std::memcpy(frame.bytes, unknownBytes.data(), unknownBytes.size());
  frame.length = unknownBytes.size();
  frame.reception.packetLength = unknownBytes.size();
  const auto unknown = processor.process(frame);
  TEST_ASSERT_EQUAL_UINT8(
      static_cast<uint8_t>(PacketDisposition::kUnknownNodeIgnored),
      static_cast<uint8_t>(unknown.disposition));
  TEST_ASSERT_TRUE(queue.events.empty());
  TEST_ASSERT_FALSE(processor.latest().available);

  pairing.unpair();
  TEST_ASSERT_FALSE(pairing.paired());
}

void test_configuration_validation() {
  GatewayConfig config;
  config.setDefaults("GTH-GW-AABBCCDDEEFF");
  TEST_ASSERT_TRUE(validateConfig(config));
  config.radio.spreadingFactor = 13U;
  TEST_ASSERT_FALSE(validateConfig(config));
  config.setDefaults("bad id");
  TEST_ASSERT_FALSE(validateConfig(config));
  config.setDefaults("GTH-GW-AABBCCDDEEFF");
  std::strcpy(config.backendBaseUrl, "ftp://example.test");
  TEST_ASSERT_FALSE(validateConfig(config));
  std::strcpy(config.backendBaseUrl, "https://example.test?token=bad");
  TEST_ASSERT_FALSE(validateConfig(config));
  std::strcpy(config.backendBaseUrl, "http:///missing-host");
  TEST_ASSERT_FALSE(validateConfig(config));
  std::strcpy(config.backendBaseUrl, "https://example.test:invalid");
  TEST_ASSERT_FALSE(validateConfig(config));
  std::strcpy(config.backendBaseUrl, "http://192.168.4.2:3000");
  TEST_ASSERT_TRUE(validateConfig(config));
  config.setDefaults("GTH-GW-AABBCCDDEEFF");
  config.backendBatchSize = 51U;
  TEST_ASSERT_FALSE(validateConfig(config));
  config.setDefaults("GTH-GW-AABBCCDDEEFF");
  std::strcpy(config.wifiSsid, "field-network");
  std::strcpy(config.wifiPassword, "short");
  TEST_ASSERT_FALSE(validateConfig(config));
  config.setDefaults("GTH-GW-AABBCCDDEEFF");
  std::strcpy(config.backendBearerToken, "token with spaces");
  TEST_ASSERT_FALSE(validateConfig(config));
  TEST_ASSERT_TRUE(protocol::nodeIdValid("A_-9"));
  TEST_ASSERT_FALSE(protocol::nodeIdValid("A B"));
}

void test_backend_payload_schema_and_response_policy() {
  GatewayIdentity identity{};
  std::strcpy(identity.gatewayId, "GTH-GW-AABBCCDDEEFF");
  std::strcpy(identity.hardwareMac, "AA:BB:CC:DD:EE:FF");
  std::strcpy(identity.firmwareVersion, "1.0.0");
  identity.bootSessionId = 1234567890U;
  QueueRecord record = queueRecord(std::vector<uint8_t>(std::begin(kNodeGolden),
                                                        std::end(kNodeGolden)),
                                   0x22222222U);
  record.recordId = 1U;
  std::vector<QueueRecord> records{record};
  std::string json;
  TEST_ASSERT_TRUE(serializeBackendBatch(identity, records, json));
  JsonDocument parsed;
  const DeserializationError jsonError = deserializeJson(parsed, json);
  TEST_ASSERT_FALSE(static_cast<bool>(jsonError));
  TEST_ASSERT_EQUAL_INT(1, parsed["schemaVersion"].as<int>());
  TEST_ASSERT_EQUAL_STRING(identity.gatewayId,
                           parsed["gateway"]["gatewayId"].as<const char*>());
  TEST_ASSERT_EQUAL_STRING(identity.hardwareMac,
                           parsed["gateway"]["hardwareMac"].as<const char*>());
  TEST_ASSERT_EQUAL_STRING(identity.firmwareVersion,
                           parsed["gateway"]["firmwareVersion"].as<const char*>());
  TEST_ASSERT_EQUAL_UINT32(identity.bootSessionId,
                           parsed["gateway"]["bootSessionId"].as<uint32_t>());
  TEST_ASSERT_TRUE(parsed["readings"][0]["gatewayReceivedAt"].isNull());
  TEST_ASSERT_FALSE(parsed["readings"][0]["gatewayTimeTrusted"].as<bool>());
  TEST_ASSERT_EQUAL_UINT32(
      0x22222222U,
      parsed["readings"][0]["gatewayBootSessionId"].as<uint32_t>());
  TEST_ASSERT_EQUAL_UINT64(
      record.reception.gatewayUptimeMs,
      parsed["readings"][0]["gatewayUptimeMs"].as<uint64_t>());
  TEST_ASSERT_FLOAT_WITHIN(
      0.001F, record.reception.rssiDbm,
      parsed["readings"][0]["rssiDbm"].as<float>());
  TEST_ASSERT_FLOAT_WITHIN(
      0.001F, record.reception.snrDb,
      parsed["readings"][0]["snrDb"].as<float>());
  TEST_ASSERT_EQUAL_INT32(
      record.reception.frequencyErrorHz,
      parsed["readings"][0]["frequencyErrorHz"].as<int32_t>());
  TEST_ASSERT_EQUAL_UINT16(
      record.reception.packetLength,
      parsed["readings"][0]["packetLength"].as<uint16_t>());
  TEST_ASSERT_EQUAL_STRING(
      "R1QBAQJOMQECAwSgsMDQAAASNAAAAuQAAALjAAP7LhHXDnQHBwAAAwIC",
      parsed["readings"][0]["rawPayloadBase64"].as<const char*>());

  record.reception.gatewayTimeTrusted = true;
  record.reception.gatewayReceivedUnixMs = 1'787'029'200'123LL;
  records = {record};
  TEST_ASSERT_TRUE(serializeBackendBatch(identity, records, json));
  TEST_ASSERT_FALSE(static_cast<bool>(deserializeJson(parsed, json)));
  TEST_ASSERT_EQUAL_STRING(
      "2026-08-18T05:00:00.123Z",
      parsed["readings"][0]["gatewayReceivedAt"].as<const char*>());
  TEST_ASSERT_TRUE(parsed["readings"][0]["gatewayTimeTrusted"].as<bool>());

  std::vector<BackendRecordResult> results;
  TEST_ASSERT_TRUE(parseBackendBatchResponse(
      "{\"receivedAt\":\"2026-08-18T00:00:00Z\",\"results\":[{\"index\":0,\"status\":\"INSERTED\"}]}",
      1U, results));
  TEST_ASSERT_EQUAL_UINT(1U, results.size());
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(BackendRecordStatus::kInserted),
                          static_cast<uint8_t>(results[0].status));
  TEST_ASSERT_TRUE(backendBatchResponseComplete(1U, results));
  TEST_ASSERT_TRUE(parseBackendBatchResponse(
      "{\"results\":[{\"index\":0,\"status\":\"DUPLICATE\"},{\"index\":1,\"status\":\"REJECTED_INVALID\",\"reason\":\"bad magic\"}]}",
      3U, results));
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(BackendRecordStatus::kDuplicate),
                          static_cast<uint8_t>(results[0].status));
  TEST_ASSERT_EQUAL_UINT8(
      static_cast<uint8_t>(BackendRecordStatus::kRejectedInvalid),
      static_cast<uint8_t>(results[1].status));
  TEST_ASSERT_EQUAL_STRING("bad magic", results[1].reason);
  TEST_ASSERT_FALSE(backendBatchResponseComplete(3U, results));
  TEST_ASSERT_TRUE(backendHttpResponseMayDequeue(200));
  TEST_ASSERT_FALSE(backendHttpResponseMayDequeue(401));
  TEST_ASSERT_FALSE(backendHttpResponseMayDequeue(503));
  TEST_ASSERT_FALSE(parseBackendBatchResponse("{\"results\":[]}", 0U, results));
  TEST_ASSERT_FALSE(parseBackendBatchResponse(
      "{\"results\":[{\"index\":0,\"status\":\"RETRY\"}]}", 1U, results));
}

}  // namespace

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_protocol_matches_node_golden_and_ack);
  RUN_TEST(test_protocol_node_id_bounds_sentinels_and_uint32);
  RUN_TEST(test_protocol_rejects_all_malformed_framing);
  RUN_TEST(test_pairing_is_explicit_and_single_node);
  RUN_TEST(test_dedup_exact_tuple_and_boot_sessions);
  RUN_TEST(test_queue_codec_core_recovery_overflow_and_corruption);
  RUN_TEST(test_queue_full_write_failure_is_explicit);
  RUN_TEST(test_packet_processor_enforces_durable_before_ack_and_pairing);
  RUN_TEST(test_packet_processor_duplicate_reacks_without_enqueue);
  RUN_TEST(test_packet_processor_keeps_discovery_and_unknown_nodes_out_of_production);
  RUN_TEST(test_configuration_validation);
  RUN_TEST(test_backend_payload_schema_and_response_policy);
  return UNITY_END();
}
