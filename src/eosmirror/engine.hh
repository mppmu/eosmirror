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

// Adapts the number of transfers running at once, interval by interval. The
// limit starts at min_limit and rises (next_transfer_limit) while the
// throughput in bytes or files beats the one measured at the previous limit
// by more than 5 %. It falls when operations of transfers on the target had
// to be retried; the interval after that only measures the lower limit. The
// throughput to beat decays by 0.5 % per interval, so that a record from an
// earlier part of the tree cannot block growth for good.
class TransferController {
 public:
  TransferController(size_t min_limit, size_t max_limit);

  size_t limit() const { return limit_; }

  // The limit after an interval with the given throughput, in bytes and
  // files per second.
  size_t update(double byte_rate, double file_rate, bool target_retries);

 private:
  size_t min_limit_, max_limit_, limit_;
  double byte_record_ = 0, file_record_ = 0;  // the throughput to beat
  bool measuring_ = false;                    // the limit was just lowered
};

// Synchronizes a target tree with a source tree, see docs/design.md.
class Engine {
 public:
  Engine(Endpoint& source, Endpoint& target, SyncOptions options, Report& report, Journal* journal,
         const Cancellation& cancel);
  ~Engine();

  // Synchronizes the whole tree. Failures of single entries go to the
  // report. The run fails when it cannot start, cannot list the top
  // directory, finds the source empty but not the target with deletion on,
  // or is cancelled: by the caller, or (also a Cancelled error) by the
  // engine after max_consecutive_failures failures in a row.
  Status run();

  // Synchronizes only the given entries, as recorded by the journal: each
  // one within its parent directory, directories with their subtrees.
  Status run(const std::vector<Failure>& entries);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace eosmirror
