// SPDX-License-Identifier: GPL-3.0-or-later
#include "eosmirror/cli.hh"

#include <cstdlib>
#include <functional>
#include <map>

namespace eosmirror {

namespace {

Error usage_error(const std::string& message) { return Error{ErrorKind::Other, message}; }

Result<long long> parse_integer(const std::string& text, const std::string& option) {
  char* end = nullptr;
  long long value = std::strtoll(text.c_str(), &end, 10);
  if (text.empty() || *end != '\0') return usage_error("invalid number for " + option + ": " + text);
  return value;
}

}  // namespace

Result<uint64_t> parse_size(const std::string& text) {
  char* end = nullptr;
  unsigned long long value = std::strtoull(text.c_str(), &end, 10);
  if (text.empty() || end == text.c_str()) return usage_error("invalid size: " + text);
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
  return static_cast<uint64_t>(value) * factor;
}

std::string usage() {
  return R"(Usage: eosmirror sync [options] SOURCE TARGET
       eosmirror failures JOURNAL
       eosmirror --version

Replicates the tree at SOURCE to TARGET: copies files that are missing or
differ in size or mtime, recreates symlinks, and sets owners, modes and
mtimes. Endpoints are local paths; XRootD and EOS URLs follow.

Options:
  -n, --dry-run            report what would be done, change nothing
      --delete             delete entries on the target that the source lacks
      --max-delete N       stop deleting after N entries (default 1000, or "unlimited")
      --no-owner           do not set owners and groups
      --no-mode            do not set modes
      --no-verify          do not compute checksums
      --verify-readback    read files on local targets back to verify them
      --fsync              fsync files on local targets before renaming them
      --rewrite-links A=B  rewrite symlink targets starting with A to start with B
      --checkers N         directory workers (default 8)
      --transfers N        file copy workers (default 8)
      --max-backlog N      queued copies before directory workers wait (default 10000)
      --buffer-size SIZE   copy buffer per transfer, e.g. 8M (default)
      --retries N          retries per operation (default 2)
      --retry-delay SEC    delay before the first retry (default 1)
      --journal FILE       record failures and finalized directories in FILE
      --resume             skip directories the journal records as finalized
      --retry-failed       process only the failures recorded in the journal
      --shard K/N          handle only the directories of shard K of N (K from 0)
      --progress SEC       print progress every SEC seconds
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
  auto integer = [&](const std::string& name, std::function<void(long long)> f) {
    value(name, [name, f](const std::string& v) -> Status {
      auto n = parse_integer(v, name);
      if (!n.ok()) return n.error();
      f(n.value());
      return Status();
    });
  };

  flag("-n", [&] { sync.dry_run = true; });
  flag("--dry-run", [&] { sync.dry_run = true; });
  flag("--delete", [&] { sync.delete_extra = true; });
  value("--max-delete", [&](const std::string& v) -> Status {
    if (v == "unlimited") {
      sync.max_delete = UINT64_MAX;
      return Status();
    }
    auto n = parse_integer(v, "--max-delete");
    if (!n.ok()) return n.error();
    sync.max_delete = static_cast<uint64_t>(n.value());
    return Status();
  });
  flag("--no-owner", [&] { sync.preserve_owner = false; });
  flag("--no-mode", [&] { sync.preserve_mode = false; });
  flag("--no-verify", [&] { sync.verify = false; });
  flag("--verify-readback", [&] { opts.endpoints.posix.verify_readback = true; });
  flag("--fsync", [&] { opts.endpoints.posix.fsync = true; });
  value("--rewrite-links", [&](const std::string& v) -> Status {
    auto eq = v.find('=');
    if (eq == std::string::npos || eq == 0) return usage_error("--rewrite-links expects FROM=TO");
    sync.link_rewrites.emplace_back(v.substr(0, eq), v.substr(eq + 1));
    return Status();
  });
  integer("--checkers", [&](long long n) { sync.checkers = static_cast<int>(n); });
  integer("--transfers", [&](long long n) { sync.transfers = static_cast<int>(n); });
  integer("--max-backlog", [&](long long n) { sync.max_backlog = static_cast<size_t>(n); });
  value("--buffer-size", [&](const std::string& v) -> Status {
    auto n = parse_size(v);
    if (!n.ok()) return n.error();
    sync.buffer_size = static_cast<size_t>(n.value());
    return Status();
  });
  integer("--retries", [&](long long n) { retries = static_cast<int>(n); });
  integer("--retry-delay", [&](long long n) {
    sync.retry.initial_delay = std::chrono::seconds(n);
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
    auto k = parse_integer(v.substr(0, slash), "--shard");
    auto n = parse_integer(v.substr(slash + 1), "--shard");
    if (!k.ok()) return k.error();
    if (!n.ok()) return n.error();
    sync.shard_index = static_cast<int>(k.value());
    sync.shard_count = static_cast<int>(n.value());
    return Status();
  });
  integer("--progress", [&](long long n) { opts.progress_seconds = static_cast<int>(n); });
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
  if (retries < 0) return usage_error("--retries must not be negative");
  sync.retry.attempts = retries + 1;
  if (sync.checkers < 1 || sync.transfers < 1) return usage_error("--checkers and --transfers must be at least 1");
  if (sync.shard_count < 1 || sync.shard_index < 0 || sync.shard_index >= sync.shard_count)
    return usage_error("--shard K/N needs 0 <= K < N");
  if ((sync.resume || opts.retry_failed) && opts.journal.empty())
    return usage_error("--resume and --retry-failed need --journal");
  if (sync.resume && opts.retry_failed) return usage_error("--resume and --retry-failed exclude each other");
  return opts;
}

}  // namespace eosmirror
