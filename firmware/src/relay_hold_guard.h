#pragma once
#include <stdint.h>

// No GPIO or persistent storage here. A new instance always starts OFF.
class RelayHoldGuard {
 public:
  static constexpr uint32_t COOLDOWN_MS = 2000;
  bool high() const { return high_; }
  uint32_t cooldownRemaining(uint32_t now) const {
    const uint32_t elapsed = uint32_t(now - stoppedAt_);
    return stopped_ && elapsed < COOLDOWN_MS ? COOLDOWN_MS - elapsed : 0;
  }
  bool on(uint32_t now) {
    if (high_) return true;  // Repeated requests never interrupt an active supply.
    if (cooldownRemaining(now)) return false;
    high_ = true;
    return true;
  }
  void off(uint32_t now) {
    if (high_) { stopped_ = true; stoppedAt_ = now; }
    high_ = false;  // OFF is always available, including during cooldown.
  }
 private:
  bool high_ = false, stopped_ = false;
  uint32_t stoppedAt_ = 0;
};

inline bool relayHoldSelfTest() {
  RelayHoldGuard g;
  if (g.high() || g.cooldownRemaining(0) || !g.on(10)) return false;
  if (!g.high() || !g.on(20) || !g.on(60010) || !g.high()) return false;
  g.off(60020);
  if (g.high() || g.on(60021) || g.cooldownRemaining(60021) != 1999) return false;
  g.off(61000);  // Repeated OFF must not move the cooldown deadline.
  if (g.on(62019) || !g.on(62020)) return false;
  g.off(62021);
  if (g.high()) return false;
  RelayHoldGuard reboot;
  if (reboot.high() || !reboot.on(0)) return false;
  RelayHoldGuard wrap;
  const uint32_t start = UINT32_MAX - 100;
  if (!wrap.on(start)) return false;
  wrap.off(start + 1);
  if (wrap.on(start + 2000) || !wrap.on(start + 2001)) return false;
  return true;
}
