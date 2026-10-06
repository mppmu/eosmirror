// SPDX-License-Identifier: GPL-3.0-or-later
#include <csignal>
#include <cstdio>
#include <thread>

#include "eosmirror/cli.hh"
#include "eosmirror/engine.hh"
#include "eosmirror/journal.hh"
#include "eosmirror/log.hh"
#include "eosmirror/version.hh"

using namespace eosmirror;

namespace {

enum ExitCode { kOk = 0, kFailures = 1, kUsage = 2, kFatal = 3, kInterrupted = 130 };

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
    std::printf("%s\t%s\t%s\n", std::string(to_string(f.type)).c_str(), f.path.c_str(),
                f.error.describe().c_str());
  return kOk;
}

int sync(const CliOptions& opts) {
  auto source = make_endpoint(opts.source, opts.endpoints);
  if (!source.ok()) {
    log::error(source.error().describe());
    return kFatal;
  }
  auto target = make_endpoint(opts.target, opts.endpoints);
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

  log::info("eosmirror ", EOSMIRROR_VERSION, (opts.sync.dry_run ? " dry run: " : ": "),
            source.value()->describe(), " -> ", target.value()->describe());
  Status status = opts.retry_failed ? engine.run(journal->failures()) : engine.run();
  finished = true;
  if (progress.joinable()) progress.join();

  if (journal && !opts.sync.dry_run) {
    if (Status s = journal->end_run(status.ok()); !s.ok()) log::error(s.error().describe());
  }

  std::fputs(report.summary(opts.sync.dry_run).c_str(), stdout);
  if (!status.ok()) {
    log::error(status.error().describe());
    return status.error().kind == ErrorKind::Cancelled ? kInterrupted : kFatal;
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
    case Command::Sync: return sync(opts);
  }
  return kUsage;
}
