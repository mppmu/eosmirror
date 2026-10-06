// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <memory>
#include <vector>

#include "eosmirror/cancellation.hh"
#include "eosmirror/endpoint.hh"
#include "eosmirror/error.hh"
#include "eosmirror/journal.hh"
#include "eosmirror/options.hh"
#include "eosmirror/report.hh"

namespace eosmirror {

// The adaptive transfer limit after an interval: down by a quarter when
// operations were retried, else up by half (at least 2) when throughput
// improved, within [min_limit, max_limit].
size_t next_transfer_limit(size_t limit, size_t min_limit, size_t max_limit, bool retried,
                           bool improved);

// Synchronizes a target tree with a source tree, see docs/design.md.
class Engine {
 public:
  Engine(Endpoint& source, Endpoint& target, SyncOptions options, Report& report, Journal* journal,
         const Cancellation& cancel);
  ~Engine();

  // Synchronizes the whole tree. Fails only when the run cannot start or was
  // cancelled; failures of single entries go to the report.
  Status run();

  // Synchronizes only the given entries, as recorded by the journal: each
  // one within its parent directory, directories with their subtrees.
  Status run(const std::vector<Failure>& entries);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace eosmirror
