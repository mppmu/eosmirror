// SPDX-License-Identifier: GPL-3.0-or-later
#include <algorithm>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <utility>

#include "eosmirror/cli.hh"
#include "eosmirror/engine.hh"
#include "eosmirror/journal.hh"
#include "eosmirror/log.hh"
#include "eosmirror/progress_display.hh"
#include "eosmirror/selftest.hh"
#include "eosmirror/version.hh"

using namespace eosmirror;

namespace {

enum ExitCode { kOk = 0, kFailures = 1, kUsage = 2, kFatal = 3, kStopped = 4, kInterrupted = 130 };

Cancellation g_cancel;
volatile sig_atomic_t g_signals = 0;

void on_signal(int) {
  g_cancel.request();
  g_signals = g_signals + 1;
  if (g_signals >= 2) _exit(kInterrupted);
}

int list_failures(const std::string& file) {
  auto opened = Journal::open_any(file);
  if (!opened.ok()) {
    std::fprintf(stderr, "eosmirror: %s\n", opened.error().describe().c_str());
    return kFatal;
  }
  std::fprintf(stderr, "failures of %s -> %s\n", opened.value()->source().c_str(),
               opened.value()->target().c_str());
  for (const Failure& f : opened.value()->failures())
    std::printf("%s\t%s\t%s\n", std::string(to_string(f.type)).c_str(), printable(f.path).c_str(),
                printable(f.error.describe()).c_str());
  return kOk;
}

int selftest(const CliOptions& opts) {
  auto target = make_endpoint(opts.target, opts.endpoints);
  if (!target.ok()) {
    log::error(target.error().describe());
    return kFatal;
  }
  Capabilities caps = target.value()->capabilities();
  std::printf("Self-test of %s\n", target.value()->describe().c_str());
  std::printf("  mtimes: %s, symlinks: %s, owners: %s, checksum: %s\n",
              caps.can_set_mtime ? "yes" : "no (files are compared by size only)",
              caps.has_symlinks ? "yes" : "no (skipped)",
              caps.can_set_owner ? "yes" : "no (needs --no-owner)",
              std::string(to_string(caps.checksum)).c_str());
  SelftestReport report = run_selftest(*target.value(), opts.sync.preserve_owner);
  for (const SelftestCheck& c : report.checks)
    std::printf("  %-7s %s%s%s\n", c.skipped ? "skip" : c.ok ? "ok" : "FAILED", c.name.c_str(),
                c.message.empty() ? "" : ": ", c.message.c_str());
  bool usable = report.ok() && (caps.can_set_owner || !opts.sync.preserve_owner);
  std::printf("%s\n", usable ? "The target is ready." : "The target is not ready for a run.");
  return usable ? kOk : kFailures;
}

// A specification split into the part with the path and the opaque
// parameters of a URL ("?..."), which belong to every directory on it.
std::pair<std::string, std::string> split_spec(const std::string& spec) {
  auto scheme = spec.find("://");
  if (scheme == std::string::npos || spec.compare(0, scheme, "file") == 0) return {spec, ""};
  auto query = spec.find('?');
  if (query == std::string::npos) return {spec, ""};
  return {spec.substr(0, query), spec.substr(query)};
}

// The specification of the directory above the one given ("" above the
// root). A relative local path without a slash is in the current directory.
std::string parent_spec(const std::string& spec) {
  auto [path, cgi] = split_spec(spec);
  // Where the absolute path starts: after file://, or at the second slash
  // after the server of other URLs (root://host//path).
  size_t path_start = 0;
  if (auto scheme = path.find("://"); scheme != std::string::npos) {
    bool file = path.compare(0, scheme, "file") == 0;
    path_start = file ? scheme + 3 : path.find("//", scheme + 3);
    if (path_start == std::string::npos) return "";
    if (!file) path_start += 1;
  }
  while (path.size() > path_start + 1 && path.back() == '/') path.pop_back();
  auto slash = path.rfind('/');
  if (slash == std::string::npos) return path == "." || path == ".." ? "" : ".";
  if (slash < path_start || path.size() == path_start + 1) return "";
  return path.substr(0, std::max(slash, path_start + 1)) + cgi;
}

// The last path component of a specification.
std::string spec_name(const std::string& spec) {
  auto [path, cgi] = split_spec(spec);
  while (path.size() > 1 && path.back() == '/') path.pop_back();
  return path.substr(path.rfind('/') + 1);
}

// Creates the target's missing ancestors, like mkdir -p; the engine creates
// the target directory itself.
Status ensure_parents(const std::string& spec, const EndpointSettings& settings, bool dry_run) {
  std::string parent = parent_spec(spec);
  if (parent.empty()) return {};
  auto ep = make_endpoint(parent, settings);
  if (!ep.ok()) return ep.error();
  auto st = ep.value()->stat("");
  if (st.ok()) {
    if (st.value().type != EntryType::Directory)
      return Error{ErrorKind::NotADirectory, ep.value()->describe() + " is not a directory"};
    return {};
  }
  if (st.error().kind != ErrorKind::NotFound) return st.error();
  Status above = ensure_parents(parent, settings, dry_run);
  if (!above.ok()) return above;
  log::info(dry_run ? "would create " : "creating ", ep.value()->describe());
  if (dry_run) return {};
  auto grand = make_endpoint(parent_spec(parent), settings);
  if (!grand.ok()) return grand.error();
  Status made = grand.value()->mkdir(spec_name(parent), 0755);
  if (made.ok() || made.error().kind != ErrorKind::Exists) return made;
  // Shards started together race for it.
  st = ep.value()->stat("");
  return st.ok() && st.value().type == EntryType::Directory ? Status() : made;
}

int sync(const CliOptions& opts) {
  auto source = make_endpoint(opts.source, opts.endpoints);
  if (!source.ok()) {
    log::error(source.error().describe());
    return kFatal;
  }
  // A path below /eos is resolved first, so that an error names it rather
  // than one of its parents.
  auto target_spec = eos_path_url(opts.target, opts.endpoints.mgm, std::getenv("EOS_MGM_URL"));
  if (!target_spec.ok()) {
    log::error(target_spec.error().describe());
    return kFatal;
  }
  if (Status s = ensure_parents(target_spec.value(), opts.endpoints, opts.sync.dry_run); !s.ok()) {
    log::error(s.error().describe());
    return kFatal;
  }
  auto target = make_endpoint(target_spec.value(), opts.endpoints);
  if (!target.ok()) {
    log::error(target.error().describe());
    return kFatal;
  }

  // A dry run reads the journal (for --retry-failed) but records nothing.
  std::unique_ptr<Journal> journal;
  if (!opts.journal.empty()) {
    std::string shard = std::to_string(opts.sync.shard_index) + "/" +
                        std::to_string(opts.sync.shard_count);
    auto opened = Journal::open(opts.journal, source.value()->describe(),
                                target.value()->describe(), shard);
    if (!opened.ok()) {
      log::error(opened.error().describe());
      return kFatal;
    }
    journal = std::move(opened).value();
    if (!opts.sync.dry_run) {
      if (Status s = journal->begin_run(opts.sync.resume); !s.ok()) {
        log::error(s.error().describe());
        return kFatal;
      }
    }
  }

  std::signal(SIGINT, on_signal);
  std::signal(SIGTERM, on_signal);

  Report report;
  Engine engine(*source.value(), *target.value(), opts.sync, report, journal.get(), g_cancel);

  std::atomic<bool> finished{false};
  std::thread progress;
  if (opts.progress_seconds > 0) {
    progress = std::thread([&] {
      auto interval = std::chrono::seconds(opts.progress_seconds);
      auto next = std::chrono::steady_clock::now() + interval;
      while (!finished) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        if (std::chrono::steady_clock::now() >= next) {
          log::info(report.progress());
          next += interval;
        }
      }
    });
  }

  std::string title = std::string("eosmirror ") + (opts.sync.dry_run ? "dry run: " : "") +
                      source.value()->describe() + " -> " + target.value()->describe();
  log::info(title);
  // Live bars on a terminal, unless plain progress lines were asked for or
  // the run is quiet.
  std::unique_ptr<ProgressDisplay> display;
  if (opts.progress_seconds == 0 && opts.log_level >= LogLevel::Info &&
      ProgressDisplay::suitable(STDERR_FILENO)) {
    display = std::make_unique<ProgressDisplay>(report, title);
    display->start();
  }
  Status status = opts.retry_failed ? engine.run(journal->failures()) : engine.run();
  finished = true;
  if (progress.joinable()) progress.join();
  if (display) display->stop();

  if (journal && !opts.sync.dry_run) {
    if (Status s = journal->end_run(status.ok()); !s.ok()) log::error(s.error().describe());
  }

  std::fputs(report.summary(opts.sync.dry_run).c_str(), stdout);
  if (!status.ok()) {
    log::error(status.error().describe());
    if (status.error().kind != ErrorKind::Cancelled) return kFatal;
    // Without a signal, the engine stopped the run after failures in a row.
    return g_cancel.requested() ? kInterrupted : kStopped;
  }
  return report.failure_count() > 0 ? kFailures : kOk;
}

}  // namespace

int main(int argc, char** argv) {
  auto parsed = parse_command_line(argc, argv);
  if (!parsed.ok()) {
    std::fprintf(stderr, "eosmirror: %s\n\n%s", parsed.error().message.c_str(), usage().c_str());
    return kUsage;
  }
  const CliOptions& opts = parsed.value();
  log::set_level(opts.log_level);
  switch (opts.command) {
    case Command::Version: std::printf("eosmirror %s\n", EOSMIRROR_VERSION); return kOk;
    case Command::Help: std::fputs(usage().c_str(), stdout); return kOk;
    case Command::Failures: return list_failures(opts.journal);
    case Command::Selftest: return selftest(opts);
    case Command::Sync: return sync(opts);
  }
  return kUsage;
}
