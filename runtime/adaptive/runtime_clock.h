#pragma once

// Monotonic time for the V0.6 runtime. Plain C++17.
//
// Every runtime timer (dwell, cooldown, scheduling, staleness) reads an
// IMonotonicClock, never the wall clock, so a clock adjustment cannot shorten
// or extend a gate. Tests inject a ManualClock and advance it explicitly,
// which makes every gate decision deterministic.

#include <atomic>
#include <chrono>
#include <cstdint>

namespace aafr::runtime {

class IMonotonicClock {
 public:
  virtual ~IMonotonicClock() = default;
  // Nanoseconds since an arbitrary fixed origin; never decreases.
  virtual std::int64_t NowNs() const = 0;
};

class SteadyClock final : public IMonotonicClock {
 public:
  std::int64_t NowNs() const override {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
  }
};

// Test clock: starts at `start_ns` and moves only when told to.
class ManualClock final : public IMonotonicClock {
 public:
  explicit ManualClock(std::int64_t start_ns = 1'000'000'000) : now_(start_ns) {}
  std::int64_t NowNs() const override { return now_.load(); }
  void AdvanceMs(double ms) { now_ += static_cast<std::int64_t>(ms * 1'000'000.0); }
  void SetNs(std::int64_t ns) {
    if (ns > now_.load()) now_ = ns;
  }

 private:
  std::atomic<std::int64_t> now_;
};

inline double NsToMs(std::int64_t ns) { return static_cast<double>(ns) / 1'000'000.0; }
inline std::int64_t MsToNs(double ms) { return static_cast<std::int64_t>(ms * 1'000'000.0); }

}  // namespace aafr::runtime
