// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <atomic>
#include <chrono>
#include <thread>

namespace eosmirror {

// A stop request shared by all workers. Signal handlers may call request(),
// so waiting polls instead of using condition variables.
class Cancellation {
 public:
  void request() { requested_.store(true, std::memory_order_relaxed); }
  bool requested() const { return requested_.load(std::memory_order_relaxed); }

  // Sleeps for the duration unless cancelled first. Returns true if cancelled.
  bool wait(std::chrono::milliseconds duration) const {
    auto deadline = std::chrono::steady_clock::now() + duration;
    while (!requested()) {
      auto now = std::chrono::steady_clock::now();
      if (now >= deadline) return false;
      auto slice = std::min<std::chrono::steady_clock::duration>(deadline - now,
                                                                 std::chrono::milliseconds(100));
      std::this_thread::sleep_for(slice);
    }
    return true;
  }

 private:
  std::atomic<bool> requested_{false};
};

}  // namespace eosmirror
