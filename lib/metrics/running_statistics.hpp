#pragma once

#include <stdint.h>

namespace gathra::gateway {

struct RunningStatisticsSnapshot {
  uint64_t count = 0;
  uint64_t minimum = 0;
  uint64_t maximum = 0;
  double average = 0.0;
};

// Constant-memory incremental mean with exact uint64 min/max. Samples are
// microseconds for ACK metrics, but the accumulator is unit-agnostic.
class RunningStatistics {
 public:
  void record(uint64_t sample);
  RunningStatisticsSnapshot snapshot() const;

 private:
  uint64_t count_ = 0;
  uint64_t minimum_ = 0;
  uint64_t maximum_ = 0;
  double average_ = 0.0;
};

struct AckLatencyStatistics {
  RunningStatistics rxToStart{};
  RunningStatistics rxToComplete{};
  RunningStatistics txDuration{};

  void record(uint64_t rxToStartUs, uint64_t rxToCompleteUs);
};

}  // namespace gathra::gateway
