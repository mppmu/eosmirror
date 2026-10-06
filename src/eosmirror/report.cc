// SPDX-License-Identifier: GPL-3.0-or-later
#include "eosmirror/report.hh"

#include <cstdio>
#include <sstream>

#include "eosmirror/log.hh"

namespace eosmirror {

namespace {

constexpr size_t kKeptFailures = 1000;

}  // namespace

Report::Report() : start_(std::chrono::steady_clock::now()) {}

void Report::add_failure(Failure failure) {
  stats.failures.fetch_add(1, std::memory_order_relaxed);
  log::warn(to_string(failure.type), " ", failure.path.empty() ? "." : failure.path, ": ",
            failure.error.describe());
  std::lock_guard lock(mutex_);
  if (failures_.size() < kKeptFailures) failures_.push_back(std::move(failure));
}

std::vector<Failure> Report::failures() const {
  std::lock_guard lock(mutex_);
  return failures_;
}

std::chrono::steady_clock::duration Report::elapsed() const {
  return std::chrono::steady_clock::now() - start_;
}

std::string format_bytes(uint64_t bytes) {
  const char* units[] = {"B", "KiB", "MiB", "GiB", "TiB", "PiB"};
  double value = static_cast<double>(bytes);
  int unit = 0;
  while (value >= 1024 && unit < 5) {
    value /= 1024;
    ++unit;
  }
  char buf[32];
  if (unit == 0)
    std::snprintf(buf, sizeof buf, "%llu B", static_cast<unsigned long long>(bytes));
  else
    std::snprintf(buf, sizeof buf, "%.1f %s", value, units[unit]);
  return buf;
}

std::string format_duration(std::chrono::steady_clock::duration d) {
  auto secs = std::chrono::duration_cast<std::chrono::seconds>(d).count();
  char buf[32];
  if (secs < 60)
    std::snprintf(buf, sizeof buf, "%llds", static_cast<long long>(secs));
  else if (secs < 3600)
    std::snprintf(buf, sizeof buf, "%lldm%02llds", static_cast<long long>(secs / 60),
                  static_cast<long long>(secs % 60));
  else
    std::snprintf(buf, sizeof buf, "%lldh%02lldm", static_cast<long long>(secs / 3600),
                  static_cast<long long>(secs % 3600 / 60));
  return buf;
}

std::string Report::progress() const {
  auto secs = std::chrono::duration<double>(elapsed()).count();
  uint64_t bytes = stats.bytes_copied.load();
  std::ostringstream s;
  s << "copied " << stats.files_copied.load() << " files (" << format_bytes(bytes) << ", "
    << format_bytes(secs > 0 ? static_cast<uint64_t>(static_cast<double>(bytes) / secs) : 0)
    << "/s), unchanged " << stats.files_unchanged.load() << ", dirs " << stats.dirs_listed.load()
    << ", failed " << stats.failures.load() << ", " << format_duration(elapsed());
  return s.str();
}

std::string Report::summary(bool dry_run) const {
  std::ostringstream s;
  const char* would = dry_run ? "would be " : "";
  s << (dry_run ? "Dry run summary" : "Summary") << " (" << format_duration(elapsed()) << ")\n"
    << "  directories: " << stats.dirs_listed.load() << " listed, " << stats.dirs_created.load()
    << " " << would << "created\n"
    << "  files: " << stats.files_copied.load() << " " << would << "copied ("
    << format_bytes(stats.bytes_copied.load()) << "), " << stats.files_unchanged.load()
    << " unchanged\n"
    << "  symlinks: " << stats.symlinks_created.load() << " " << would << "created, "
    << stats.symlinks_unchanged.load() << " unchanged, " << stats.symlinks_skipped.load()
    << " skipped\n"
    << "  metadata " << would << "fixed: " << stats.metadata_fixed.load() << "\n"
    << "  special files skipped: " << stats.specials_skipped.load() << "\n"
    << "  extra entries on target: " << stats.extras.load() << ", " << would
    << "deleted: " << stats.deleted.load() << ", stale temporaries: " << stats.stale_temps.load()
    << "\n"
    << "  retries: " << stats.retries.load() << ", failures: " << stats.failures.load() << "\n";
  auto failures = this->failures();
  if (!failures.empty()) {
    s << "Failures" << (stats.failures.load() > failures.size() ? " (first " + std::to_string(failures.size()) + ")" : "")
      << ":\n";
    for (const auto& f : failures)
      s << "  " << to_string(f.type) << " " << (f.path.empty() ? "." : f.path) << ": "
        << f.error.describe() << "\n";
  }
  return s.str();
}

}  // namespace eosmirror
