// SPDX-License-Identifier: GPL-3.0-or-later
#include "eosmirror/cli.hh"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <functional>
#include <limits>
#include <map>
#include <optional>

namespace eosmirror {

namespace {

Error usage_error(const std::string& message) { return Error{ErrorKind::Other, message}; }

Result<long long> parse_integer(const std::string& text, const std::string& option) {
  char* end = nullptr;
  errno = 0;
  long long value = std::strtoll(text.c_str(), &end, 10);
  if (text.empty() || *end != '\0' || errno == ERANGE)
    return usage_error("invalid number for " + option + ": " + text);
  return value;
}

// An integer within [min, max], so that it fits where it goes.
Result<long long> parse_integer(const std::string& text, const std::string& option, long long min,
                                long long max) {
  auto n = parse_integer(text, option);
  if (n.ok() && (n.value() < min || n.value() > max))
    return usage_error(option + " must be between " + std::to_string(min) + " and " +
                       std::to_string(max));
  return n;
}

constexpr long long kMaxInt = std::numeric_limits<int>::max();
// Threads of each pool at most.
constexpr long long kMaxThreads = 1024;

}  // namespace

Result<uint64_t> parse_size(const std::string& text) {
  char* end = nullptr;
  errno = 0;
  unsigned long long value = std::strtoull(text.c_str(), &end, 10);
  // strtoull takes a sign, and wraps a minus around.
  if (text.empty() || !std::isdigit(static_cast<unsigned char>(text[0])) || errno == ERANGE)
    return usage_error("invalid size: " + text);
  uint64_t factor = 1;
  std::string suffix(end);
  if (suffix == "k" || suffix == "K")
    factor = 1ull << 10;
  else if (suffix == "M")
    factor = 1ull << 20;
  else if (suffix == "G")
    factor = 1ull << 30;
  else if (suffix == "T")
    factor = 1ull << 40;
  else if (!suffix.empty())
    return usage_error("invalid size: " + text);
  if (value > std::numeric_limits<uint64_t>::max() / factor)
    return usage_error("invalid size: " + text);
  return static_cast<uint64_t>(value) * factor;
}

RunOptions result_options(const CliOptions& opts) {
  auto yes = [](bool on) { return std::string(on ? "yes" : "no"); };
  // Lengths first, so that no two lists of rewrites read the same.
  std::string rewrites;
  for (const auto& [from, to] : opts.sync.link_rewrites)
    rewrites += std::to_string(from.size()) + ":" + from + std::to_string(to.size()) + ":" + to;
  return {{"--size-only", yes(opts.sync.size_only)},
          {"--delete", yes(opts.sync.delete_extra)},
          {"--keep-emptied", yes(opts.sync.keep_emptied)},
          {"--one-file-system", yes(opts.sync.one_file_system)},
          {"--no-owner", yes(!opts.sync.preserve_owner)},
          {"--no-mode", yes(!opts.sync.preserve_mode)},
          {"--no-verify", yes(!opts.sync.verify)},
          {"--require-checksum", yes(opts.sync.require_checksum)},
          {"--verify-readback", yes(opts.endpoints.posix.verify_readback)},
          {"--rewrite-links", rewrites}};
}

std::string usage() {
  return R"(Usage: eosmirror sync [options] SOURCE TARGET
       eosmirror selftest [--no-owner] [--mgm URL] TARGET
       eosmirror failures JOURNAL
       eosmirror --version

sync replicates the tree at SOURCE to TARGET: copies files that are missing
or differ in size or mtime, recreates symlinks, and sets owners, modes and
mtimes. selftest checks, in a temporary directory under TARGET, that
everything a run needs works there. Endpoints are local paths or XRootD
URLs (root://host//path, roots:// for TLS); an EOS instance is recognized,
or named with eos://. Paths below /eos are on the EOS instance of --mgm or
EOS_MGM_URL, not on a local FUSE mount (name that as file:///eos/...).

Options:
  -n, --dry-run            report what would be done, change nothing
      --size-only          compare files by size only, not by mtime, as targets
                           that store no mtimes need (plain XRootD)
      --delete             delete entries on the target that the source lacks
      --keep-emptied       with --delete, delete nothing in directories that are
                           empty on the source, as an unmounted file system looks
      --max-delete N       stop deleting after N entries (default unlimited)
      --target-without-symlinks
                           state that a plain XRootD target has no symlinks,
                           which --delete needs there: XRootD cannot show them,
                           and deletes what they point to
      --no-owner           do not set owners and groups
      --no-mode            do not set modes
      --no-verify          do not compute checksums
      --require-checksum   fail copies that neither the target nor the source can
                           verify by checksum
      --verify-readback    read files on local targets back to verify them
      --no-fsync           give copies on local targets their mtimes at once,
                           without syncing; by default they have mtime 0 until a
                           sync of their file system (every 30 s and at the end)
                           has made them durable
      --rewrite-links A=B  rewrite symlink targets starting with A to start with B
  -x, --one-file-system    skip directories of a local source that are on other
                           file systems than SOURCE, which are kept on the target
                           (no effect on remote sources)
      --checkers N         directory workers (default 8, at most 1024)
      --min-transfers N    copies running at once at the start, and at least
                           (default 4); the limit rises while throughput improves,
                           and falls back when a rise did not pay or the target
                           fails operations
      --max-transfers N    copies running at once at most (default 64, at most
                           1024); with the default windows and buffer size, 64
                           copies hold up to 64 x 9 x 8 MiB = 4.5 GiB of buffers
      --transfers N        exactly N copies running at once
      --write-window N     writes per file in flight to XRootD and EOS (default 4),
                           in order; safe for erasure-coded EOS files only while
                           the FSTs keep EOS's default "xrootd.async off";
                           --write-window 1 opts out
      --read-window N      reads per file in flight from XRootD and EOS sources
                           (default 4)
      --shared-connection  send the requests of all transfers and directory workers
                           over one connection per XRootD or EOS server, instead
                           of over connections of each one's own
      --max-backlog N      queued copies before directory workers wait (default 10000)
      --buffer-size SIZE   size of the chunks read and written, e.g. 8M (default);
                           a transfer holds at most both windows plus one
      --retries N          retries per operation (default 2)
      --retry-delay SEC    delay before the first retry (default 1)
      --max-consecutive-failures N
                           stop the run after N failures in a row, without a
                           success in between (default 1000, or "unlimited")
      --mgm URL            the EOS MGM for paths below /eos, root://host[:port]
                           (default: EOS_MGM_URL)
      --journal FILE       record failures and finalized directories in FILE
      --resume             continue an interrupted run, with its options: skip the
                           directories the journal records as finalized
      --retry-failed       process only the failures recorded in the journal
      --shard K/N          handle only the directories of shard K of N (K from 0)
      --progress SEC       print a progress line every SEC seconds instead of the
                           live display (which needs a terminal and is off with -q)
  -v, --verbose            debug output
  -q, --quiet              warnings and errors only
  -h, --help               this help
)";
}

Result<CliOptions> parse_command_line(int argc, char** argv) {
  CliOptions opts;
  std::vector<std::string> args(argv + 1, argv + argc);
  if (args.empty()) return usage_error("no command given");

  const std::string& command = args[0];
  if (command == "--version") {
    opts.command = Command::Version;
    return opts;
  }
  if (command == "-h" || command == "--help" || command == "help") {
    opts.command = Command::Help;
    return opts;
  }
  if (command == "failures") {
    if (args.size() != 2) return usage_error("usage: eosmirror failures JOURNAL");
    opts.command = Command::Failures;
    opts.journal = args[1];
    return opts;
  }
  if (command == "selftest") {
    opts.command = Command::Selftest;
    for (size_t i = 1; i < args.size(); ++i) {
      if (args[i] == "--no-owner")
        opts.sync.preserve_owner = false;
      else if (args[i] == "--mgm" && i + 1 == args.size())
        return usage_error("--mgm needs a value");
      else if (args[i] == "--mgm")
        opts.endpoints.mgm = args[++i];
      else if (args[i].starts_with("--mgm="))
        opts.endpoints.mgm = args[i].substr(6);
      else if (args[i] == "-v" || args[i] == "--verbose")
        opts.log_level = LogLevel::Debug;
      else if (args[i][0] == '-' && args[i].size() > 1)
        return usage_error("unknown option for selftest: " + args[i]);
      else if (opts.target.empty())
        opts.target = args[i];
      else
        return usage_error("selftest takes one TARGET");
    }
    if (opts.target.empty()) return usage_error("selftest needs a TARGET");
    return opts;
  }
  if (command != "sync") return usage_error("unknown command: " + command);
  opts.command = Command::Sync;

  std::vector<std::string> positional;
  SyncOptions& sync = opts.sync;
  int retries = 2;

  using Handler = std::function<Status(const std::string&)>;
  std::map<std::string, std::pair<bool, Handler>> handlers;  // option -> (takes value, handler)
  auto flag = [&](const std::string& name, std::function<void()> f) {
    handlers[name] = {false, [f](const std::string&) {
                        f();
                        return Status();
                      }};
  };
  auto value = [&](const std::string& name, Handler f) { handlers[name] = {true, f}; };
  auto integer = [&](const std::string& name, long long min, long long max,
                     std::function<void(long long)> f) {
    value(name, [name, min, max, f](const std::string& v) -> Status {
      auto n = parse_integer(v, name, min, max);
      if (!n.ok()) return n.error();
      f(n.value());
      return Status();
    });
  };

  flag("-n", [&] { sync.dry_run = true; });
  flag("--dry-run", [&] { sync.dry_run = true; });
  flag("--size-only", [&] { sync.size_only = true; });
  flag("--delete", [&] { sync.delete_extra = true; });
  flag("--keep-emptied", [&] { sync.keep_emptied = true; });
  flag("--target-without-symlinks", [&] { sync.target_without_symlinks = true; });
  // An option that takes a count, or "unlimited" for the given value.
  auto limit = [&](const std::string& name, uint64_t& out, uint64_t unlimited) {
    value(name, [name, &out, unlimited](const std::string& v) -> Status {
      if (v == "unlimited") {
        out = unlimited;
        return Status();
      }
      auto n = parse_integer(v, name);
      if (!n.ok()) return n.error();
      if (n.value() < 0) return usage_error(name + " must not be negative");
      out = static_cast<uint64_t>(n.value());
      return Status();
    });
  };
  limit("--max-delete", sync.max_delete, UINT64_MAX);
  limit("--max-consecutive-failures", sync.max_consecutive_failures, 0);
  flag("-x", [&] { sync.one_file_system = true; });
  flag("--one-file-system", [&] { sync.one_file_system = true; });
  flag("--no-owner", [&] { sync.preserve_owner = false; });
  flag("--no-mode", [&] { sync.preserve_mode = false; });
  flag("--no-verify", [&] { sync.verify = false; });
  flag("--require-checksum", [&] { sync.require_checksum = true; });
  flag("--verify-readback", [&] { opts.endpoints.posix.verify_readback = true; });
  flag("--no-fsync", [&] { opts.endpoints.posix.sync = false; });
  value("--rewrite-links", [&](const std::string& v) -> Status {
    auto eq = v.find('=');
    if (eq == std::string::npos || eq == 0) return usage_error("--rewrite-links expects FROM=TO");
    sync.link_rewrites.emplace_back(v.substr(0, eq), v.substr(eq + 1));
    return Status();
  });
  integer("--checkers", 1, kMaxThreads, [&](long long n) { sync.checkers = static_cast<int>(n); });
  // Given on the command line: --transfers N, or --min-transfers and
  // --max-transfers.
  std::optional<int> transfers, min_transfers, max_transfers;
  integer("--transfers", 1, kMaxThreads, [&](long long n) { transfers = static_cast<int>(n); });
  integer("--min-transfers", 1, kMaxThreads,
          [&](long long n) { min_transfers = static_cast<int>(n); });
  integer("--max-transfers", 1, kMaxThreads,
          [&](long long n) { max_transfers = static_cast<int>(n); });
  integer("--write-window", 1, kMaxInt,
          [&](long long n) { opts.endpoints.xrootd.write_window = static_cast<int>(n); });
  integer("--read-window", 1, kMaxInt,
          [&](long long n) { opts.endpoints.xrootd.read_window = static_cast<int>(n); });
  flag("--shared-connection", [&] { opts.endpoints.xrootd.shared_connection = true; });
  integer("--max-backlog", 0, std::numeric_limits<long long>::max(),
          [&](long long n) { sync.max_backlog = static_cast<size_t>(n); });
  value("--buffer-size", [&](const std::string& v) -> Status {
    auto n = parse_size(v);
    if (!n.ok()) return n.error();
    sync.buffer_size = static_cast<size_t>(n.value());
    return Status();
  });
  // The attempts are one more.
  integer("--retries", 0, kMaxInt - 1, [&](long long n) { retries = static_cast<int>(n); });
  // In milliseconds.
  integer("--retry-delay", 0, std::numeric_limits<long long>::max() / 1000,
          [&](long long n) { sync.retry.initial_delay = std::chrono::seconds(n); });
  value("--mgm", [&](const std::string& v) -> Status {
    opts.endpoints.mgm = v;
    return Status();
  });
  value("--journal", [&](const std::string& v) -> Status {
    opts.journal = v;
    return Status();
  });
  flag("--resume", [&] { sync.resume = true; });
  flag("--retry-failed", [&] { opts.retry_failed = true; });
  value("--shard", [&](const std::string& v) -> Status {
    auto slash = v.find('/');
    if (slash == std::string::npos) return usage_error("--shard expects K/N");
    auto k = parse_integer(v.substr(0, slash), "--shard", 0, kMaxInt);
    auto n = parse_integer(v.substr(slash + 1), "--shard", 1, kMaxInt);
    if (!k.ok()) return k.error();
    if (!n.ok()) return n.error();
    sync.shard_index = static_cast<int>(k.value());
    sync.shard_count = static_cast<int>(n.value());
    return Status();
  });
  integer("--progress", 0, kMaxInt, [&](long long n) { opts.progress_seconds = static_cast<int>(n); });
  flag("-v", [&] { opts.log_level = LogLevel::Debug; });
  flag("--verbose", [&] { opts.log_level = LogLevel::Debug; });
  flag("-q", [&] { opts.log_level = LogLevel::Warn; });
  flag("--quiet", [&] { opts.log_level = LogLevel::Warn; });
  flag("-h", [&] { opts.command = Command::Help; });
  flag("--help", [&] { opts.command = Command::Help; });

  for (size_t i = 1; i < args.size(); ++i) {
    const std::string& arg = args[i];
    if (arg == "--") {
      positional.insert(positional.end(), args.begin() + static_cast<long>(i) + 1, args.end());
      break;
    }
    if (arg.size() < 2 || arg[0] != '-') {
      positional.push_back(arg);
      continue;
    }
    std::string name = arg, inline_value;
    bool has_inline = false;
    if (auto eq = arg.find('='); arg.compare(0, 2, "--") == 0 && eq != std::string::npos) {
      name = arg.substr(0, eq);
      inline_value = arg.substr(eq + 1);
      has_inline = true;
    }
    auto it = handlers.find(name);
    if (it == handlers.end()) return usage_error("unknown option: " + arg);
    auto& [takes_value, handler] = it->second;
    std::string v;
    if (takes_value) {
      if (has_inline) {
        v = inline_value;
      } else if (i + 1 < args.size()) {
        v = args[++i];
      } else {
        return usage_error(name + " needs a value");
      }
    } else if (has_inline) {
      return usage_error(name + " takes no value");
    }
    Status s = handler(v);
    if (!s.ok()) return s.error();
  }
  if (opts.command == Command::Help) return opts;

  if (positional.size() != 2) return usage_error("sync needs exactly SOURCE and TARGET");
  opts.source = positional[0];
  opts.target = positional[1];
  sync.retry.attempts = retries + 1;
  if (transfers && (min_transfers || max_transfers))
    return usage_error("--transfers excludes --min-transfers and --max-transfers");
  if (transfers) min_transfers = max_transfers = transfers;
  if (min_transfers && max_transfers && *min_transfers > *max_transfers)
    return usage_error("--min-transfers must not exceed --max-transfers");
  // One of them given alone moves the other's default where needed.
  if (min_transfers) sync.min_transfers = *min_transfers;
  if (max_transfers) sync.max_transfers = *max_transfers;
  if (!min_transfers) sync.min_transfers = std::min(sync.min_transfers, sync.max_transfers);
  if (!max_transfers) sync.max_transfers = std::max(sync.max_transfers, sync.min_transfers);
  if (sync.buffer_size > (1u << 30)) return usage_error("--buffer-size must be at most 1G");
  if (sync.shard_count < 1 || sync.shard_index < 0 || sync.shard_index >= sync.shard_count)
    return usage_error("--shard K/N needs 0 <= K < N");
  if (sync.keep_emptied && !sync.delete_extra)
    return usage_error("--keep-emptied needs --delete");
  if ((sync.resume || opts.retry_failed) && opts.journal.empty())
    return usage_error("--resume and --retry-failed need --journal");
  if (sync.resume && opts.retry_failed) return usage_error("--resume and --retry-failed exclude each other");
  return opts;
}

}  // namespace eosmirror
