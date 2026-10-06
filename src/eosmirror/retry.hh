// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <atomic>
#include <cstdint>
#include <utility>

#include "eosmirror/cancellation.hh"
#include "eosmirror/error.hh"
#include "eosmirror/options.hh"

namespace eosmirror {

// Runs an operation returning a Result or Status, repeating it with backoff
// while it fails with a transient error. Counts every retry.
template <class Op>
auto with_retries(const RetryPolicy& policy, const Cancellation& cancel,
                  std::atomic<uint64_t>& retries, Op&& op) -> decltype(op()) {
  for (int attempt = 1;; ++attempt) {
    auto result = op();
    if (result.ok() || !is_transient(result.error().kind) || attempt >= policy.attempts ||
        cancel.requested())
      return result;
    retries.fetch_add(1, std::memory_order_relaxed);
    if (cancel.wait(policy.delay(attempt))) return result;
  }
}

}  // namespace eosmirror
