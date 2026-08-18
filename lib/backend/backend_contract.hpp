#pragma once

#include <stddef.h>
#include <stdint.h>

#include <string>
#include <vector>

#include "build_config.hpp"
#include "queue_record.hpp"

namespace gathra::gateway {

struct GatewayIdentity {
  char gatewayId[build::kGatewayIdCapacity]{};
  char hardwareMac[18]{};
  char firmwareVersion[17]{};
  uint32_t bootSessionId = 0;
};

enum class BackendRecordStatus : uint8_t {
  kInserted,
  kDuplicate,
  kRejectedInvalid,
};

struct BackendRecordResult {
  size_t index = 0;
  BackendRecordStatus status = BackendRecordStatus::kRejectedInvalid;
  char reason[97]{};
};

bool serializeBackendBatch(const GatewayIdentity& identity,
                           const std::vector<QueueRecord>& records,
                           std::string& json);
bool parseBackendBatchResponse(const char* json, size_t expectedRecordCount,
                               std::vector<BackendRecordResult>& results);
bool backendHttpResponseMayDequeue(int httpStatus);
bool backendBatchResponseComplete(
    size_t expectedRecordCount,
    const std::vector<BackendRecordResult>& results);
const char* backendRecordStatusName(BackendRecordStatus status);

}  // namespace gathra::gateway
