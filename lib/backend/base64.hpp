#pragma once

#include <stddef.h>
#include <stdint.h>

#include <string>
#include <vector>

namespace gathra::gateway {

std::string base64Encode(const uint8_t* input, size_t length);
bool base64Decode(const char* input, std::vector<uint8_t>& output);

}  // namespace gathra::gateway
