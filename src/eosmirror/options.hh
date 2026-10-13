// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace eosmirror {

struct RetryPolicy {
  int attempts = 3;  // including the first one
  std::chrono::milliseconds initial_delay{1000};
  double factor = 4.0;
  std::chrono::milliseconds max_delay{60000};

  // The delay before the given retry (1 = the first retry).
  std::chrono::milliseconds delay(int retry) const {
    double d = static_cast<double>(initial_delay.count());
    for (int i = 1; i < retry; ++i) d *= factor;
    auto ms = static_cast<std::chrono::milliseconds::rep>(d);
    return std::chrono::milliseconds(ms < max_delay.count() ? ms : max_delay.count());
  }
};

struct SyncOptions {
  int checkers = 8;  // directory workers
  // Copies running at once: min_transfers at the start, then adapted to
  // throughput and target errors up to max_transfers (TransferController).
  // Equal bounds fix the number.
  int min_transfers = 4;
  int max_transfers = 64;
  std::chrono::seconds adapt_interval{10};
  size_t max_backlog = 10000;  // queued copies before the checkers block
  size_t buffer_size = 8u << 20;

  bool dry_run = false;
  // Compare files by size alone, not by mtime, as a target that stores no
  // mtimes needs; mtimes are still set where the target stores them.
  bool size_only = false;
  bool delete_extra = false;
  // Keep the extras of directories that are empty on the source but not on
  // the target, which delete_extra deletes like any other (the top
  // directory of a run never: an empty source stops the run).
  bool keep_emptied = false;
  uint64_t max_delete = UINT64_MAX;
  // The target has no symlinks, which delete_extra needs to be told for a
  // target that follows symlinks it cannot show.
  bool target_without_symlinks = false;
  bool preserve_owner = true;
  bool preserve_mode = true;
  bool verify = true;  // compute and compare checksums where possible
  bool require_checksum = false;  // fail copies the target cannot verify
  bool resume = false;  // skip directories the journal records as finalized
  // Skip directories of the source on other file systems than its root.
  bool one_file_system = false;

  RetryPolicy retry;
  // The run stops after this many failures without a success in between
  // (0: never), so that an outage of an endpoint does not turn every
  // remaining entry into a failure.
  uint64_t max_consecutive_failures = 1000;

  int shard_index = 0;
  int shard_count = 1;

  // Symlink targets starting with .first are rewritten to start with .second.
  std::vector<std::pair<std::string, std::string>> link_rewrites;

  // Temporaries not changed for this long (by mtime and ctime) are taken
  // for leftovers of earlier runs, and removed with --delete.
  std::chrono::seconds stale_temp_age{3600};
};

}  // namespace eosmirror
