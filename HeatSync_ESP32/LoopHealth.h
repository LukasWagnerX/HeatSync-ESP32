#pragma once
#include <stdint.h>

static constexpr uint32_t HEATSYNC_LOOP_STALL_MS = 90000;
static inline bool heatSyncLoopStalled(uint32_t now, uint32_t progressAt, bool armed) {
  return armed && (uint32_t) (now - progressAt) >= HEATSYNC_LOOP_STALL_MS;
}
