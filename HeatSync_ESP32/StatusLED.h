#pragma once
#include <stdint.h>

enum class StatusLEDMode : uint8_t { off, setup, resetPending };

inline bool statusLEDOn(StatusLEDMode mode, uint32_t now, bool resetReady = false) {
  if (resetReady) return now % 200 < 100;
  switch (mode) {
    case StatusLEDMode::setup: return now % 1000 < 500;
    case StatusLEDMode::resetPending: {
      const uint32_t phase = now % 1600;
      return phase < 120 || (phase >= 240 && phase < 360);
    }
    default: return false;
  }
}
