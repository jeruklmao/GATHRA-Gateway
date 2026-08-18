#pragma once

#include <stdint.h>

// Immutable Gateway v1 production wiring. Runtime configuration and the web
// dashboard must never override these values.
namespace gathra::gateway::board {
inline constexpr uint8_t kRadioReset = 1;
inline constexpr uint8_t kRadioDio0 = 3;
inline constexpr uint8_t kRadioSck = 4;
inline constexpr uint8_t kRadioMiso = 5;
inline constexpr uint8_t kRadioMosi = 6;
inline constexpr uint8_t kRadioCs = 7;
}  // namespace gathra::gateway::board
