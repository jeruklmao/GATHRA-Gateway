#include "gateway_config.hpp"

#include <ctype.h>
#include <math.h>
#include <string.h>

#include "protocol.hpp"

namespace gathra::gateway {
namespace {

ConfigValidationResult fail(ConfigValidationCode code, const char* message) {
  return {code, message};
}

bool validBandwidth(float value) {
  constexpr float values[] = {7.8F, 10.4F, 15.6F, 20.8F, 31.25F,
                              41.7F, 62.5F, 125.0F, 250.0F};
  for (float candidate : values) {
    if (fabsf(value - candidate) < 0.06F) return true;
  }
  return false;
}

bool identifierValid(const char* value, size_t capacity) {
  if (value == nullptr) return false;
  const size_t length = strnlen(value, capacity);
  if (length == 0U || length >= capacity) return false;
  for (size_t i = 0; i < length; ++i) {
    const unsigned char c = static_cast<unsigned char>(value[i]);
    const bool asciiAlphanumeric =
        (c >= static_cast<unsigned char>('A') &&
         c <= static_cast<unsigned char>('Z')) ||
        (c >= static_cast<unsigned char>('a') &&
         c <= static_cast<unsigned char>('z')) ||
        (c >= static_cast<unsigned char>('0') &&
         c <= static_cast<unsigned char>('9'));
    if (!(asciiAlphanumeric || c == '-' || c == '_')) return false;
  }
  return true;
}

bool boundedCString(const char* value, size_t capacity) {
  return value != nullptr && strnlen(value, capacity) < capacity;
}

bool containsControlOrSpace(const char* value) {
  if (value == nullptr) return true;
  for (const char* cursor = value; *cursor != '\0'; ++cursor) {
    const unsigned char c = static_cast<unsigned char>(*cursor);
    if (iscntrl(c) || isspace(c)) return true;
  }
  return false;
}

bool decimalPortValid(const char* begin, const char* end) {
  if (begin == nullptr || end == nullptr || begin >= end) return false;
  uint32_t port = 0;
  for (const char* cursor = begin; cursor < end; ++cursor) {
    if (*cursor < '0' || *cursor > '9') return false;
    port = port * 10U + static_cast<uint32_t>(*cursor - '0');
    if (port > 65'535U) return false;
  }
  return port > 0U;
}

bool dnsHostValid(const char* begin, const char* end) {
  if (begin == nullptr || end == nullptr || begin >= end) return false;
  bool labelStart = true;
  for (const char* cursor = begin; cursor < end; ++cursor) {
    const unsigned char c = static_cast<unsigned char>(*cursor);
    const bool alphanumeric =
        (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
        (c >= '0' && c <= '9');
    if (alphanumeric) {
      labelStart = false;
      continue;
    }
    if (c == '-' && !labelStart && cursor + 1 < end && cursor[1] != '.') {
      continue;
    }
    if (c == '.' && !labelStart && cursor[-1] != '-') {
      labelStart = true;
      continue;
    }
    return false;
  }
  return !labelStart && end[-1] != '-';
}

bool authorityValid(const char* begin, const char* end) {
  if (begin == nullptr || end == nullptr || begin >= end) return false;
  if (*begin == '[') {
    const char* closing = static_cast<const char*>(
        memchr(begin + 1, ']', static_cast<size_t>(end - begin - 1)));
    if (closing == nullptr || closing == begin + 1) return false;
    for (const char* cursor = begin + 1; cursor < closing; ++cursor) {
      const unsigned char c = static_cast<unsigned char>(*cursor);
      const bool hexadecimal =
          (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f') ||
          (c >= '0' && c <= '9');
      if (!hexadecimal && c != ':' && c != '.') return false;
    }
    if (closing + 1 == end) return true;
    return closing[1] == ':' && decimalPortValid(closing + 2, end);
  }
  const char* portSeparator = static_cast<const char*>(
      memchr(begin, ':', static_cast<size_t>(end - begin)));
  const char* hostEnd = portSeparator == nullptr ? end : portSeparator;
  if (!dnsHostValid(begin, hostEnd)) return false;
  return portSeparator == nullptr || decimalPortValid(portSeparator + 1, end);
}

}  // namespace

void GatewayConfig::setDefaults(const char* derivedGatewayId) {
  *this = GatewayConfig{};
  strncpy(gatewayId, derivedGatewayId == nullptr ? "GTH-GW-UNKNOWN" : derivedGatewayId,
          sizeof(gatewayId) - 1U);
  strncpy(backendBaseUrl, build::kDefaultBackendUrl, sizeof(backendBaseUrl) - 1U);
}

bool gatewayIdValid(const char* gatewayId) {
  return identifierValid(gatewayId, build::kGatewayIdCapacity);
}

bool backendUrlValid(const char* url) {
  if (!boundedCString(url, build::kBackendUrlCapacity)) return false;
  const size_t length = strlen(url);
  if (length < 10U) return false;
  const bool http = strncmp(url, "http://", 7U) == 0;
  const bool https = strncmp(url, "https://", 8U) == 0;
  if (!http && !https) return false;
  const char* authority = url + (https ? 8 : 7);
  if (*authority == '\0' || strchr(authority, '@') != nullptr ||
      strchr(authority, '?') != nullptr || strchr(authority, '#') != nullptr) {
    return false;
  }
  const char* path = strchr(authority, '/');
  const char* authorityEnd = path == nullptr ? url + length : path;
  if (!authorityValid(authority, authorityEnd) ||
      strchr(authority, '\\') != nullptr) {
    return false;
  }
  for (const char* cursor = url; *cursor != '\0'; ++cursor) {
    const unsigned char c = static_cast<unsigned char>(*cursor);
    if (iscntrl(c) || isspace(c)) return false;
  }
  return url[length - 1U] != '/';
}

bool radioConfigEqual(const RadioConfig& a, const RadioConfig& b) {
  return fabsf(a.frequencyMhz - b.frequencyMhz) < 0.001F &&
         fabsf(a.bandwidthKhz - b.bandwidthKhz) < 0.01F &&
         a.spreadingFactor == b.spreadingFactor &&
         a.codingRateDenominator == b.codingRateDenominator &&
         a.txPowerDbm == b.txPowerDbm && a.syncWord == b.syncWord;
}

ConfigValidationResult validateConfig(const GatewayConfig& c) {
  if (c.schemaVersion != build::kConfigSchemaVersion) {
    return fail(ConfigValidationCode::kSchema, "unsupported configuration schema");
  }
  if (!gatewayIdValid(c.gatewayId)) {
    return fail(ConfigValidationCode::kGatewayId,
                "gatewayId must be 1-48 ASCII letters, digits, '-' or '_'");
  }
  if (!boundedCString(c.wifiSsid, sizeof(c.wifiSsid)) ||
      !boundedCString(c.wifiPassword, sizeof(c.wifiPassword)) ||
      (c.wifiSsid[0] == '\0' && c.wifiPassword[0] != '\0') ||
      (c.wifiPassword[0] != '\0' && strlen(c.wifiPassword) < 8U)) {
    return fail(ConfigValidationCode::kWifi, "invalid Wi-Fi credentials");
  }
  if (c.pairedNodeId[0] != '\0' && !protocol::nodeIdValid(c.pairedNodeId)) {
    return fail(ConfigValidationCode::kNodeId,
                "pairedNodeId must be empty or 1-24 ASCII letters, digits, '-' or '_'");
  }
  if (!backendUrlValid(c.backendBaseUrl)) {
    return fail(ConfigValidationCode::kBackendUrl,
                "backend URL must be an http:// or https:// base URL without credentials or trailing slash");
  }
  if (!boundedCString(c.backendBearerToken, sizeof(c.backendBearerToken)) ||
      containsControlOrSpace(c.backendBearerToken)) {
    return fail(ConfigValidationCode::kBearerToken,
                "backend token must be bounded and contain no whitespace");
  }
  const RadioConfig& r = c.radio;
  if (!isfinite(r.frequencyMhz) || r.frequencyMhz < 410.0F ||
      r.frequencyMhz > 525.0F || !validBandwidth(r.bandwidthKhz) ||
      r.spreadingFactor < 7U || r.spreadingFactor > 12U ||
      r.codingRateDenominator < 5U || r.codingRateDenominator > 8U ||
      r.txPowerDbm < 2 || r.txPowerDbm > 20) {
    return fail(ConfigValidationCode::kRadio, "invalid SX1278 radio setting");
  }
  if (c.backendBatchSize == 0U ||
      c.backendBatchSize > build::kBackendMaximumBatchSize ||
      c.backendHttpTimeoutMs < 1'000U || c.backendHttpTimeoutMs > 30'000U ||
      c.backendInitialBackoffMs < 1'000U || c.backendInitialBackoffMs > 60'000U ||
      c.backendMaximumBackoffMs < c.backendInitialBackoffMs ||
      c.backendMaximumBackoffMs > 300'000U) {
    return fail(ConfigValidationCode::kBackendPolicy, "invalid backend batching/timeout/backoff policy");
  }
  if (c.wifiReconnectIntervalMs < 5'000U || c.wifiReconnectIntervalMs > 300'000U ||
      c.wifiFallbackAfterMs < 15'000U || c.wifiFallbackAfterMs > 3'600'000U ||
      c.wifiApGraceMs > 300'000U) {
    return fail(ConfigValidationCode::kWifiPolicy, "invalid Wi-Fi reconnect/fallback policy");
  }
  return {};
}

}  // namespace gathra::gateway
