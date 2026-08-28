#include "running_statistics.hpp"

namespace gathra::gateway {

void RunningStatistics::record(uint64_t sample) {
  ++count_;
  if (count_ == 1U) {
    minimum_ = maximum_ = sample;
    average_ = static_cast<double>(sample);
    return;
  }
  if (sample < minimum_) minimum_ = sample;
  if (sample > maximum_) maximum_ = sample;
  average_ += (static_cast<double>(sample) - average_) /
              static_cast<double>(count_);
}

RunningStatisticsSnapshot RunningStatistics::snapshot() const {
  return {count_, minimum_, maximum_, average_};
}

void AckLatencyStatistics::record(uint64_t rxToStartUs,
                                  uint64_t rxToCompleteUs) {
  if (rxToCompleteUs < rxToStartUs) return;
  rxToStart.record(rxToStartUs);
  rxToComplete.record(rxToCompleteUs);
  txDuration.record(rxToCompleteUs - rxToStartUs);
}

}  // namespace gathra::gateway
