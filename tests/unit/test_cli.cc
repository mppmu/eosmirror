// SPDX-License-Identifier: GPL-3.0-or-later
#include "eosmirror/cli.hh"

#include <vector>

#include "doctest/doctest.h"

using namespace eosmirror;

namespace {

Result<CliOptions> parse(std::vector<std::string> args) {
  std::vector<char*> argv;
  std::string prog = "eosmirror";
  argv.push_back(prog.data());
  for (auto& a : args) argv.push_back(a.data());
  return parse_command_line(static_cast<int>(argv.size()), argv.data());
}

}  // namespace

TEST_CASE("sizes are parsed with binary suffixes") {
  CHECK(parse_size("123").value() == 123);
  CHECK(parse_size("64k").value() == 64 * 1024);
  CHECK(parse_size("8M").value() == 8u << 20);
  CHECK(parse_size("2G").value() == 2ull << 30);
  CHECK_FALSE(parse_size("").ok());
  CHECK_FALSE(parse_size("8MB").ok());
  CHECK_FALSE(parse_size("x").ok());
}

TEST_CASE("the sync command line maps onto the options") {
  auto r = parse({"sync", "-n", "--delete", "--max-delete", "5", "--no-owner", "--checkers=3",
                  "--transfers", "4", "--buffer-size", "1M", "--retries", "0",
                  "--write-window", "1", "--read-window=6", "--connection-per-transfer",
                  "--rewrite-links", "/old/=/new/", "--journal", "j.db", "--resume",
                  "--shard", "1/3", "--progress", "10", "-v", "/src", "/dst"});
  REQUIRE(r.ok());
  const CliOptions& o = r.value();
  CHECK(o.command == Command::Sync);
  CHECK(o.source == "/src");
  CHECK(o.target == "/dst");
  CHECK(o.sync.dry_run);
  CHECK(o.sync.delete_extra);
  CHECK(o.sync.max_delete == 5);
  CHECK_FALSE(o.sync.preserve_owner);
  CHECK(o.sync.preserve_mode);
  CHECK(o.sync.checkers == 3);
  CHECK(o.sync.transfers == 4);
  CHECK(o.sync.buffer_size == 1u << 20);
  CHECK(o.endpoints.xrootd.write_window == 1);
  CHECK(o.endpoints.xrootd.read_window == 6);
  CHECK(o.endpoints.xrootd.connection_per_thread);
  CHECK(o.sync.retry.attempts == 1);
  REQUIRE(o.sync.link_rewrites.size() == 1);
  CHECK(o.sync.link_rewrites[0].first == "/old/");
  CHECK(o.sync.link_rewrites[0].second == "/new/");
  CHECK(o.journal == "j.db");
  CHECK(o.sync.resume);
  CHECK(o.sync.shard_index == 1);
  CHECK(o.sync.shard_count == 3);
  CHECK(o.progress_seconds == 10);
  CHECK(o.log_level == LogLevel::Debug);

  auto defaults = parse({"sync", "a", "b"});
  REQUIRE(defaults.ok());
  CHECK(defaults.value().sync.retry.attempts == 3);
  CHECK(defaults.value().sync.preserve_owner);
  CHECK(defaults.value().sync.max_delete == 1000);
  CHECK_FALSE(defaults.value().sync.dry_run);
  CHECK(defaults.value().endpoints.xrootd.write_window == 4);
  CHECK(defaults.value().endpoints.xrootd.read_window == 4);
  CHECK_FALSE(defaults.value().endpoints.xrootd.connection_per_thread);

  CHECK(defaults.value().sync.max_consecutive_failures == 1000);
  CHECK(parse({"sync", "--max-delete", "unlimited", "a", "b"}).value().sync.max_delete == UINT64_MAX);
  CHECK(parse({"sync", "--max-consecutive-failures", "10", "a", "b"})
            .value()
            .sync.max_consecutive_failures == 10);
  CHECK(parse({"sync", "--max-consecutive-failures=unlimited", "a", "b"})
            .value()
            .sync.max_consecutive_failures == 0);
  CHECK(parse({"sync", "--", "-weird", "b"}).value().source == "-weird");
}

TEST_CASE("usage errors are rejected") {
  CHECK_FALSE(parse({}).ok());
  CHECK_FALSE(parse({"frobnicate"}).ok());
  CHECK_FALSE(parse({"sync", "a"}).ok());
  CHECK_FALSE(parse({"sync", "a", "b", "c"}).ok());
  CHECK_FALSE(parse({"sync", "--bogus", "a", "b"}).ok());
  CHECK_FALSE(parse({"sync", "--checkers", "x", "a", "b"}).ok());
  CHECK_FALSE(parse({"sync", "--checkers", "a", "b"}).ok());
  CHECK_FALSE(parse({"sync", "--dry-run=yes", "a", "b"}).ok());
  CHECK_FALSE(parse({"sync", "--resume", "a", "b"}).ok());
  CHECK_FALSE(parse({"sync", "--shard", "3/3", "a", "b"}).ok());
  CHECK_FALSE(parse({"sync", "--rewrite-links", "noequals", "a", "b"}).ok());
  CHECK_FALSE(parse({"sync", "--write-window", "0", "a", "b"}).ok());
  CHECK_FALSE(parse({"sync", "--read-window", "0", "a", "b"}).ok());
  CHECK_FALSE(parse({"sync", "--buffer-size", "2G", "a", "b"}).ok());
  CHECK_FALSE(parse({"sync", "--max-delete", "-1", "a", "b"}).ok());
  CHECK_FALSE(parse({"sync", "--max-consecutive-failures", "x", "a", "b"}).ok());
  CHECK_FALSE(parse({"failures"}).ok());

  CHECK(parse({"--version"}).value().command == Command::Version);
  CHECK(parse({"--help"}).value().command == Command::Help);
  CHECK(parse({"sync", "--help"}).value().command == Command::Help);
  CHECK(parse({"failures", "j.db"}).value().journal == "j.db");
}

TEST_CASE("paths below /eos name the EOS instance of the MGM") {
  CHECK(parse({"sync", "--mgm", "root://mgm.example.org", "/eos/a", "/local"}).value().endpoints.mgm ==
        "root://mgm.example.org");
  CHECK(parse({"selftest", "--mgm=root://m", "/eos/a"}).value().endpoints.mgm == "root://m");
  CHECK_FALSE(parse({"selftest", "/eos/a", "--mgm"}).ok());

  CHECK(eos_path_url("/eos/user/x", "root://mgm:1094/", nullptr).value() ==
        "root://mgm:1094//eos/user/x");
  CHECK(eos_path_url("/eos", "", "root://env.example.org").value() == "root://env.example.org//eos");
  CHECK(eos_path_url("/eos/x", "root://option", "root://env").value() == "root://option//eos/x");
  CHECK(eos_path_url("/eos/x", "", "root://env//").value() == "root://env//eos/x");
  auto unset = eos_path_url("/eos/x", "", nullptr);
  REQUIRE_FALSE(unset.ok());
  CHECK(unset.error().message.find("EOS_MGM_URL") != std::string::npos);
  CHECK_FALSE(eos_path_url("/eos/x", "", "").ok());
  // Other paths, and the explicit local form, stay as they are.
  for (const char* spec : {"/eosuser/x", "/data/eos/x", "file:///eos/x", "root://h//eos/x", "eos"})
    CHECK(eos_path_url(spec, "", "root://env").value() == spec);
}
