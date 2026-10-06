// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "eosmirror/error.hh"
#include "eosmirror/types.hh"

namespace eosmirror {

struct Failure {
  RelPath path;
  EntryType type = EntryType::File;
  Error error;
};

using Counter = std::atomic<uint64_t>;

// Everything a run counts. Workers update the counters concurrently.
struct Stats {
  Counter dirs_listed{0};
  Counter dirs_created{0};
  Counter files_checked{0};
  Counter files_copied{0};
  Counter bytes_copied{0};
  Counter bytes_written{0};  // every chunk written, including copies still running
  Counter files_unchanged{0};
  Counter files_unverified{0};  // copied where no checksum could be compared
  Counter symlinks_created{0};
  Counter symlinks_unchanged{0};
  Counter symlinks_skipped{0};  // the target has no symlinks
  Counter metadata_fixed{0};
  Counter specials_skipped{0};
  Counter extras{0};
  Counter deleted{0};
  Counter stale_temps{0};
  Counter retries{0};
  Counter failures{0};
};

// Counters plus the failures of a run, and their presentation.
class Report {
 public:
  Report();

  Stats stats;

  // Counts a failure, logs it and keeps the first few for the summary.
  void add_failure(Failure failure);
  std::vector<Failure> failures() const;
  uint64_t failure_count() const { return stats.failures.load(); }

  std::chrono::steady_clock::duration elapsed() const;

  // A one-line progress report.
  std::string progress() const;

  // The multi-line summary printed at the end.
  std::string summary(bool dry_run) const;

 private:
  std::chrono::steady_clock::time_point start_;
  mutable std::mutex mutex_;
  std::vector<Failure> failures_;
};

std::string format_bytes(uint64_t bytes);
std::string format_duration(std::chrono::steady_clock::duration d);

}  // namespace eosmirror
