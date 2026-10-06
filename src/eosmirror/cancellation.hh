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
  Cancellation() = default;
  // A cancellation that is also requested whenever the parent is.
  explicit Cancellation(const Cancellation* parent) : parent_(parent) {}

  void request() { requested_.store(true, std::memory_order_relaxed); }
  bool requested() const {
    return requested_.load(std::memory_order_relaxed) || (parent_ && parent_->requested());
  }

  // Sleeps for the duration unless cancelled first, or until the stop flag is
  // set. Returns true if cancelled or stopped.
  bool wait(std::chrono::milliseconds duration, const std::atomic<bool>* stop = nullptr) const {
    auto deadline = std::chrono::steady_clock::now() + duration;
    while (!requested() && !(stop && stop->load())) {
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
  const Cancellation* parent_ = nullptr;
};

}  // namespace eosmirror
