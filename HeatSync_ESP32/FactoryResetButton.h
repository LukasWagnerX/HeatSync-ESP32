#pragma once
#include <stdint.h>

class FactoryResetButton {
 public:
  static constexpr uint32_t holdMilliseconds = 5000;
  static constexpr uint32_t debounceMilliseconds = 40;

  bool update(uint32_t now, bool pressed) {
    if (!initialized_) {
      initialized_ = true; rawPressed_ = stablePressed_ = pressed;
      initialReleaseRequired_ = pressed; changedAt_ = now;
      return false;
    }
    if (pressed != rawPressed_) { rawPressed_ = pressed; changedAt_ = now; }
    if (uint32_t(now - changedAt_) < debounceMilliseconds) return false;
    if (stablePressed_ != rawPressed_) {
      stablePressed_ = rawPressed_;
      if (stablePressed_) {
        pressedAt_ = changedAt_; ready_ = false;
      } else {
        if (initialReleaseRequired_) { initialReleaseRequired_ = false; ready_ = false; return false; }
        const bool trigger = ready_ || uint32_t(changedAt_ - pressedAt_) >= holdMilliseconds;
        ready_ = false;
        return trigger;
      }
    }
    if (stablePressed_ && !initialReleaseRequired_ && uint32_t(now - pressedAt_) >= holdMilliseconds) ready_ = true;
    return false;
  }
  bool ready() const { return ready_; }

 private:
  bool initialized_ = false;
  bool rawPressed_ = false;
  bool stablePressed_ = false;
  bool initialReleaseRequired_ = false;
  bool ready_ = false;
  uint32_t changedAt_ = 0;
  uint32_t pressedAt_ = 0;
};
