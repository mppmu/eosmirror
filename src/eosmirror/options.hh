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
  int checkers = 8;         // directory workers
  int transfers = 8;        // file copy workers
  size_t max_backlog = 10000;  // queued copies before the checkers block
  size_t buffer_size = 8u << 20;

  bool dry_run = false;
  bool delete_extra = false;
  uint64_t max_delete = 1000;
  bool preserve_owner = true;
  bool preserve_mode = true;
  bool verify = true;  // compute and compare checksums where possible
  bool resume = false;  // skip directories the journal records as finalized

  RetryPolicy retry;

  int shard_index = 0;
  int shard_count = 1;

  // Symlink targets starting with .first are rewritten to start with .second.
  std::vector<std::pair<std::string, std::string>> link_rewrites;

  // Temporary files of earlier runs older than this are removed with --delete.
  std::chrono::seconds stale_temp_age{3600};
};

}  // namespace eosmirror
