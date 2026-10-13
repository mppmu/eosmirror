// SPDX-License-Identifier: GPL-3.0-or-later
#include "eosmirror/cli.hh"

#include <utility>
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
  // Nothing wraps around.
  for (const char* text : {"-1", "+1", " 1", "18446744073709551616", "16777216T"})
    CHECK_FALSE(parse_size(text).ok());
  CHECK(parse_size("16777215T").value() == 16777215ull << 40);
}

TEST_CASE("the sync command line maps onto the options") {
  auto r = parse({"sync", "-n", "--delete", "--keep-emptied", "--max-delete", "5", "--no-owner", "--checkers=3",
                  "--transfers", "4", "--buffer-size", "1M", "--retries", "0",
                  "--write-window", "1", "--read-window=6", "--shared-connection",
                  "--rewrite-links", "/old/=/new/", "--journal", "j.db", "--resume",
                  "--shard", "1/3", "--progress", "10", "-v", "/src", "/dst"});
  REQUIRE(r.ok());
  const CliOptions& o = r.value();
  CHECK(o.command == Command::Sync);
  CHECK(o.source == "/src");
  CHECK(o.target == "/dst");
  CHECK(o.sync.dry_run);
  CHECK(o.sync.delete_extra);
  CHECK(o.sync.keep_emptied);
  CHECK_FALSE(o.sync.target_without_symlinks);
  CHECK_FALSE(o.sync.size_only);
  CHECK_FALSE(o.sync.one_file_system);
  CHECK(parse({"sync", "-x", "a", "b"}).value().sync.one_file_system);
  CHECK(parse({"sync", "--one-file-system", "a", "b"}).value().sync.one_file_system);
  CHECK(parse({"sync", "--size-only", "a", "b"}).value().sync.size_only);
  CHECK(parse({"sync", "--target-without-symlinks", "a", "b"}).value().sync.target_without_symlinks);
  CHECK(o.sync.max_delete == 5);
  CHECK_FALSE(o.sync.preserve_owner);
  CHECK(o.sync.preserve_mode);
  CHECK(o.sync.checkers == 3);
  CHECK(o.sync.min_transfers == 4);
  CHECK(o.sync.max_transfers == 4);
  CHECK(o.sync.buffer_size == 1u << 20);
  CHECK(o.endpoints.xrootd.write_window == 1);
  CHECK(o.endpoints.xrootd.read_window == 6);
  CHECK(o.endpoints.xrootd.shared_connection);
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
  CHECK(defaults.value().sync.max_delete == UINT64_MAX);
  CHECK_FALSE(defaults.value().sync.keep_emptied);
  CHECK_FALSE(defaults.value().sync.dry_run);
  CHECK(defaults.value().endpoints.xrootd.write_window == 4);
  CHECK(defaults.value().endpoints.xrootd.read_window == 4);
  CHECK_FALSE(defaults.value().endpoints.xrootd.shared_connection);
  CHECK(defaults.value().sync.min_transfers == 4);
  CHECK(defaults.value().sync.max_transfers == 64);

  CHECK(defaults.value().sync.max_consecutive_failures == 1000);
  CHECK(parse({"sync", "--max-delete", "unlimited", "a", "b"}).value().sync.max_delete == UINT64_MAX);
  CHECK(parse({"sync", "--max-delete", "1000", "a", "b"}).value().sync.max_delete == 1000);
  CHECK(parse({"sync", "--max-consecutive-failures", "10", "a", "b"})
            .value()
            .sync.max_consecutive_failures == 10);
  CHECK(parse({"sync", "--max-consecutive-failures=unlimited", "a", "b"})
            .value()
            .sync.max_consecutive_failures == 0);
  CHECK(parse({"sync", "--", "-weird", "b"}).value().source == "-weird");
}

TEST_CASE("transfers are a range to adapt within, or a fixed number") {
  auto transfers = [](std::vector<std::string> args) {
    args.insert(args.begin(), "sync");
    args.push_back("a");
    args.push_back("b");
    auto r = parse(args);
    REQUIRE(r.ok());
    return std::pair{r.value().sync.min_transfers, r.value().sync.max_transfers};
  };
  CHECK(transfers({"--min-transfers", "2", "--max-transfers", "16"}) == std::pair{2, 16});
  CHECK(transfers({"--min-transfers=8"}) == std::pair{8, 64});
  CHECK(transfers({"--max-transfers", "32"}) == std::pair{4, 32});
  CHECK(transfers({"--transfers", "12"}) == std::pair{12, 12});
  CHECK(transfers({"--min-transfers", "5", "--max-transfers", "5"}) == std::pair{5, 5});
  // The other bound's default gives way to one given alone.
  CHECK(transfers({"--max-transfers", "2"}) == std::pair{2, 2});
  CHECK(transfers({"--min-transfers", "100"}) == std::pair{100, 100});

  CHECK_FALSE(parse({"sync", "--min-transfers", "8", "--max-transfers", "4", "a", "b"}).ok());
  CHECK_FALSE(parse({"sync", "--transfers", "8", "--max-transfers", "16", "a", "b"}).ok());
  CHECK_FALSE(parse({"sync", "--min-transfers", "2", "--transfers", "8", "a", "b"}).ok());
  CHECK_FALSE(parse({"sync", "--transfers", "0", "a", "b"}).ok());
  CHECK_FALSE(parse({"sync", "--max-transfers", "0", "a", "b"}).ok());
  CHECK_FALSE(parse({"sync", "--no-adaptive", "a", "b"}).ok());
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
  CHECK_FALSE(parse({"sync", "--keep-emptied", "a", "b"}).ok());
  CHECK_FALSE(parse({"sync", "--delete", "--delete-emptied", "a", "b"}).ok());
  CHECK_FALSE(parse({"sync", "--max-consecutive-failures", "x", "a", "b"}).ok());
  // Numbers out of the range of where they go, rather than cut to fit.
  for (std::vector<std::string> args :
       {std::vector<std::string>{"--shard", "4294967296/2"}, {"--shard", "1/4294967298"},
        {"--max-backlog", "-1"}, {"--checkers", "4294967297"}, {"--transfers", "-4294967295"},
        {"--retries", "2147483647"}, {"--retry-delay", "-1"}, {"--progress", "-5"},
        {"--write-window", "4294967297"}, {"--max-delete", "99999999999999999999"},
        {"--buffer-size", "-1"}, {"--checkers", "1025"}, {"--transfers", "1025"},
        {"--min-transfers", "1025"}, {"--max-transfers", "1025"}}) {
    std::string given = args[0] + " " + args[1];
    INFO(given);
    args.insert(args.begin(), "sync");
    args.push_back("a");
    args.push_back("b");
    CHECK_FALSE(parse(args).ok());
  }
  CHECK(parse({"sync", "--retries", "2147483646", "a", "b"}).value().sync.retry.attempts ==
        2147483647);
  CHECK(parse({"sync", "--checkers", "1024", "--transfers", "1024", "a", "b"}).ok());
  CHECK_FALSE(parse({"failures"}).ok());

  CHECK(parse({"--version"}).value().command == Command::Version);
  CHECK(parse({"--help"}).value().command == Command::Help);
  CHECK(parse({"sync", "--help"}).value().command == Command::Help);
  CHECK(parse({"failures", "j.db"}).value().journal == "j.db");
}

TEST_CASE("displayed URLs carry no credentials") {
  CHECK(display_url("root://host:1094//eos/dir?authz=secret&x=1") == "root://host:1094//eos/dir");
  CHECK(display_url("root://user:secret@host//eos?xrd.wantprot=krb5") == "root://user@host//eos");
  CHECK(display_url("root://user@host:1094//eos") == "root://user@host:1094//eos");
  CHECK(display_url("root://host//data/a@b:c") == "root://host//data/a@b:c");
  CHECK(display_url("root://host:1094") == "root://host:1094");

  // Nor do the errors about endpoints that cannot be made.
  std::vector<std::string> specs = {"unknown://user:secret@host//x?authz=secret", "/eos/x?authz=secret"};
#ifndef EOSMIRROR_HAVE_XROOTD
  specs.push_back("root://user:secret@host//x?authz=secret");
#endif
  for (const std::string& spec : specs) {
    INFO("endpoint ", spec);
    auto made = make_endpoint(spec, EndpointSettings{});
    REQUIRE_FALSE(made.ok());
    CHECK(made.error().describe().find("secret") == std::string::npos);
  }
}

TEST_CASE("the options that decide what a run makes of the target") {
  auto names = [](const CliOptions& a, const CliOptions& b) {
    std::vector<std::string> differing;
    RunOptions x = result_options(a), y = result_options(b);
    REQUIRE(x.size() == y.size());
    for (size_t i = 0; i < x.size(); ++i)
      if (x[i] != y[i]) differing.push_back(x[i].first);
    return differing;
  };
  auto plain = parse({"sync", "a", "b"}).value();
  auto tuned = parse({"sync", "--checkers", "3", "--max-delete", "5", "a", "b"});
  CHECK(names(plain, tuned.value()).empty());
  CHECK(names(plain, parse({"sync", "--delete", "--no-owner", "a", "b"}).value()) ==
        std::vector<std::string>{"--delete", "--no-owner"});
  auto rewritten = parse({"sync", "--rewrite-links", "/a=/b", "a", "b"}).value();
  CHECK(names(rewritten, parse({"sync", "--rewrite-links", "/a=/c", "a", "b"}).value()) ==
        std::vector<std::string>{"--rewrite-links"});
  CHECK(names(rewritten, parse({"sync", "--rewrite-links", "/a=/b", "--rewrite-links", "x=y", "a",
                                "b"}).value()) == std::vector<std::string>{"--rewrite-links"});
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
  // A '?' would make the rest of the path opaque parameters of the URL.
  auto cut = eos_path_url("/eos/project?archive", "", "root://env");
  REQUIRE_FALSE(cut.ok());
  CHECK(cut.error().message.find("'?'") != std::string::npos);
  // Other paths, and the explicit local form, stay as they are.
  for (const char* spec : {"/eosuser/x", "/data/eos/x", "file:///eos/x", "root://h//eos/x", "eos"})
    CHECK(eos_path_url(spec, "", "root://env").value() == spec);
}
