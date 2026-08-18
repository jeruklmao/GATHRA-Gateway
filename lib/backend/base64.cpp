#include "base64.hpp"

#include <string.h>

namespace gathra::gateway {
namespace {

constexpr char kAlphabet[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

int decodeValue(char c) {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (c == '+') return 62;
  if (c == '/') return 63;
  return -1;
}

}  // namespace

std::string base64Encode(const uint8_t* input, size_t length) {
  if (input == nullptr && length != 0U) return {};
  std::string output;
  output.reserve(((length + 2U) / 3U) * 4U);
  for (size_t index = 0; index < length; index += 3U) {
    const uint32_t a = input[index];
    const uint32_t b = index + 1U < length ? input[index + 1U] : 0U;
    const uint32_t c = index + 2U < length ? input[index + 2U] : 0U;
    const uint32_t value = (a << 16U) | (b << 8U) | c;
    output.push_back(kAlphabet[(value >> 18U) & 0x3FU]);
    output.push_back(kAlphabet[(value >> 12U) & 0x3FU]);
    output.push_back(index + 1U < length ? kAlphabet[(value >> 6U) & 0x3FU] : '=');
    output.push_back(index + 2U < length ? kAlphabet[value & 0x3FU] : '=');
  }
  return output;
}

bool base64Decode(const char* input, std::vector<uint8_t>& output) {
  output.clear();
  if (input == nullptr) return false;
  const size_t length = strlen(input);
  if (length == 0U || length % 4U != 0U) return false;
  output.reserve(length / 4U * 3U);
  for (size_t index = 0; index < length; index += 4U) {
    const bool final = index + 4U == length;
    const int a = decodeValue(input[index]);
    const int b = decodeValue(input[index + 1U]);
    const int c = input[index + 2U] == '=' ? -2 : decodeValue(input[index + 2U]);
    const int d = input[index + 3U] == '=' ? -2 : decodeValue(input[index + 3U]);
    if (a < 0 || b < 0 || c == -1 || d == -1 ||
        (!final && (c == -2 || d == -2)) || c == -2 && d != -2) {
      output.clear();
      return false;
    }
    const uint32_t value = (static_cast<uint32_t>(a) << 18U) |
                           (static_cast<uint32_t>(b) << 12U) |
                           (static_cast<uint32_t>(c < 0 ? 0 : c) << 6U) |
                           static_cast<uint32_t>(d < 0 ? 0 : d);
    output.push_back(static_cast<uint8_t>(value >> 16U));
    if (c != -2) output.push_back(static_cast<uint8_t>(value >> 8U));
    if (d != -2) output.push_back(static_cast<uint8_t>(value));
  }
  return base64Encode(output.data(), output.size()) == input;
}

}  // namespace gathra::gateway
