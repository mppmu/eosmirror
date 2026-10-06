// SPDX-License-Identifier: GPL-3.0-or-later
#include "eosmirror/engine.hh"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <thread>

#include "doctest/doctest.h"
#include "eosmirror/copy.hh"
#include "eosmirror/posix_endpoint.hh"
#include "fake_endpoint.hh"
#include "stderr_capture.hh"
#include "temp_dir.hh"

using namespace eosmirror;
namespace fs = std::filesystem;

namespace {

SyncOptions test_options() {
  SyncOptions o;
  o.checkers = 2;
  o.transfers = 2;
  o.adaptive = false;
  o.buffer_size = 64 * 1024;
  o.retry.attempts = 3;
  o.retry.initial_delay = std::chrono::milliseconds(1);
  o.retry.factor = 1;
  o.preserve_owner = is_root();
  return o;
}

Status run_sync(Endpoint& src, Endpoint& dst, const SyncOptions& options, Report& report,
                Journal* journal = nullptr) {
  Cancellation cancel;
  Engine engine(src, dst, options, report, journal, cancel);
  return engine.run();
}

void set_mtime(const std::string& path, int64_t sec, int32_t nsec) {
  struct timespec times[2] = {{sec, nsec}, {sec, nsec}};
  REQUIRE(utimensat(AT_FDCWD, path.c_str(), times, AT_SYMLINK_NOFOLLOW) == 0);
}

// Checks that every source entry has a matching target entry.
void compare_trees(PosixEndpoint& src, PosixEndpoint& dst, const RelPath& dir = "") {
  auto s = src.list(dir);
  auto d = dst.list(dir);
  REQUIRE(s.ok());
  REQUIRE(d.ok());
  for (const Entry& e : s.value()) {
    if (e.type == EntryType::Other) continue;
    RelPath path = join(dir, e.name);
    INFO("entry ", path);
    auto it = std::find_if(d.value().begin(), d.value().end(),
                           [&](const Entry& x) { return x.name == e.name; });
    REQUIRE(it != d.value().end());
    CHECK(it->type == e.type);
    CHECK(it->mtime == e.mtime);
    if (e.type != EntryType::Symlink) CHECK(it->mode == e.mode);
    if (e.type == EntryType::File) {
      CHECK(it->size == e.size);
      std::ifstream a(src.absolute(path), std::ios::binary), b(dst.absolute(path), std::ios::binary);
      CHECK(std::string(std::istreambuf_iterator<char>(a), {}) ==
            std::string(std::istreambuf_iterator<char>(b), {}));
    }
    if (e.type == EntryType::Symlink) CHECK(it->link_target == e.link_target);
    if (e.type == EntryType::Directory) compare_trees(src, dst, path);
  }
}

std::string pattern(size_t n) {
  std::string s(n, '\0');
  for (size_t i = 0; i < n; ++i) s[i] = static_cast<char>('a' + (i * 7) % 26);
  return s;
}

// A source tree with every entry type, sizes around the buffer size, and
// distinct modes and mtimes.
void make_source_tree(const TempDir& tmp) {
  tmp.write_file("src/a.txt", "hello", 0640);
  tmp.write_file("src/big.bin", pattern(300 * 1024 + 17), 0600);
  tmp.write_file("src/empty", "", 0444);
  tmp.write_file("src/sub/nested/deep/file", pattern(1000), 0664);
  chmod(tmp.sub("src/sub").c_str(), 0750);
  chmod(tmp.sub("src/sub/nested").c_str(), 02775);
  REQUIRE(symlink("a.txt", tmp.sub("src/link_rel").c_str()) == 0);
  REQUIRE(symlink("/nonexistent/abs", tmp.sub("src/link_abs").c_str()) == 0);
  REQUIRE(symlink("../big.bin", tmp.sub("src/sub/link_up").c_str()) == 0);
  REQUIRE(mkfifo(tmp.sub("src/fifo").c_str(), 0600) == 0);
  int64_t t = 1600000000;
  for (const char* p : {"src/a.txt", "src/big.bin", "src/empty", "src/sub/nested/deep/file",
                        "src/link_rel", "src/link_abs", "src/sub/link_up", "src/sub/nested/deep",
                        "src/sub/nested", "src/sub", "src"})
    set_mtime(tmp.sub(p), t++, 123456789);
}

}  // namespace

TEST_CASE("FS to FS: a tree is replicated and reruns change nothing") {
  TempDir tmp;
  make_source_tree(tmp);
  PosixEndpoint src(tmp.sub("src"));
  PosixEndpoint dst(tmp.sub("dst"));
  SyncOptions options = test_options();

  Report first;
  REQUIRE(run_sync(src, dst, options, first).ok());
  CHECK(first.stats.files_copied == 4);
  CHECK(first.stats.bytes_copied == 5 + 300 * 1024 + 17 + 1000);
  CHECK(first.stats.symlinks_created == 3);
  CHECK(first.stats.dirs_created == 4);
  CHECK(first.stats.dirs_listed == 4);
  CHECK(first.stats.specials_skipped == 1);
  CHECK(first.stats.failures == 0);
  CHECK(first.stats.metadata_fixed == 0);
  compare_trees(src, dst);

  Report second;
  REQUIRE(run_sync(src, dst, options, second).ok());
  CHECK(second.stats.files_copied == 0);
  CHECK(second.stats.files_unchanged == 4);
  CHECK(second.stats.symlinks_created == 0);
  CHECK(second.stats.symlinks_unchanged == 3);
  CHECK(second.stats.dirs_created == 0);
  CHECK(second.stats.metadata_fixed == 0);
  CHECK(second.stats.failures == 0);

  SUBCASE("a changed mtime or size triggers a copy") {
    set_mtime(tmp.sub("src/a.txt"), 1700000000, 0);
    tmp.write_file("src/empty", "no longer", 0444);
    set_mtime(tmp.sub("src/empty"), 1600000002, 123456789);  // same mtime, other size
    Report r;
    REQUIRE(run_sync(src, dst, options, r).ok());
    CHECK(r.stats.files_copied == 2);
    CHECK(r.stats.files_unchanged == 2);
    compare_trees(src, dst);
  }

  SUBCASE("changed modes are fixed without copying") {
    chmod(tmp.sub("src/a.txt").c_str(), 0600);
    chmod(tmp.sub("src/sub").c_str(), 0700);
    set_mtime(tmp.sub("src/sub"), 1600000009, 123456789);  // chmod changed ctime only
    Report r;
    REQUIRE(run_sync(src, dst, options, r).ok());
    CHECK(r.stats.files_copied == 0);
    CHECK(r.stats.metadata_fixed == 2);
    compare_trees(src, dst);
  }

  SUBCASE("a changed symlink target is replaced") {
    unlink(tmp.sub("src/link_rel").c_str());
    REQUIRE(symlink("empty", tmp.sub("src/link_rel").c_str()) == 0);
    set_mtime(tmp.sub("src/link_rel"), 1600000004, 123456789);
    set_mtime(tmp.sub("src"), 1600000010, 123456789);
    Report r;
    REQUIRE(run_sync(src, dst, options, r).ok());
    CHECK(r.stats.symlinks_created == 1);
    CHECK(r.stats.symlinks_unchanged == 2);
    compare_trees(src, dst);
  }

  SUBCASE("extras are counted, and deleted only with the option") {
    tmp.write_file("dst/extra_file", "x");
    tmp.write_file("dst/extra_dir/inner/f", "y");
    Report r;
    REQUIRE(run_sync(src, dst, options, r).ok());
    CHECK(r.stats.extras == 2);
    CHECK(r.stats.deleted == 0);
    CHECK(fs::exists(tmp.sub("dst/extra_file")));

    SyncOptions del = options;
    del.delete_extra = true;
    Report r2;
    REQUIRE(run_sync(src, dst, del, r2).ok());
    CHECK(r2.stats.extras == 2);
    CHECK(r2.stats.deleted == 4);
    CHECK_FALSE(fs::exists(tmp.sub("dst/extra_file")));
    CHECK_FALSE(fs::exists(tmp.sub("dst/extra_dir")));
    compare_trees(src, dst);
  }

  SUBCASE("the deletion cap stops deleting and is reported") {
    tmp.write_file("dst/extra1", "x");
    tmp.write_file("dst/extra2", "x");
    tmp.write_file("dst/extra3", "x");
    SyncOptions del = options;
    del.delete_extra = true;
    del.max_delete = 2;
    Report r;
    REQUIRE(run_sync(src, dst, del, r).ok());
    CHECK(r.stats.deleted == 2);
    CHECK(r.stats.failures == 1);
    int remaining = 0;
    for (const char* p : {"dst/extra1", "dst/extra2", "dst/extra3"}) remaining += fs::exists(tmp.sub(p));
    CHECK(remaining == 1);
  }

  SUBCASE("stale temporaries of earlier runs are not extras and are cleaned with --delete") {
    tmp.write_file("dst/.a.txt.eosmirror-0123456789ab", "partial");
    set_mtime(tmp.sub("dst/.a.txt.eosmirror-0123456789ab"), 1500000000, 0);
    tmp.write_file("dst/.fresh.eosmirror-0123456789ab", "in progress");
    Report r;
    REQUIRE(run_sync(src, dst, options, r).ok());
    CHECK(r.stats.extras == 0);
    CHECK(r.stats.stale_temps == 1);

    SyncOptions del = options;
    del.delete_extra = true;
    Report r2;
    REQUIRE(run_sync(src, dst, del, r2).ok());
    CHECK(r2.stats.deleted == 1);
    CHECK_FALSE(fs::exists(tmp.sub("dst/.a.txt.eosmirror-0123456789ab")));
    CHECK(fs::exists(tmp.sub("dst/.fresh.eosmirror-0123456789ab")));
  }

  SUBCASE("a type conflict is an error unless deletion is allowed") {
    fs::remove(tmp.sub("src/a.txt"));
    tmp.write_file("src/a.txt/inside", "now a directory");
    set_mtime(tmp.sub("src"), 1600000010, 123456789);
    Report r;
    REQUIRE(run_sync(src, dst, options, r).ok());
    CHECK(r.stats.failures == 1);
    CHECK(r.failures()[0].error.kind == ErrorKind::Exists);
    CHECK(fs::is_regular_file(tmp.sub("dst/a.txt")));

    SyncOptions del = options;
    del.delete_extra = true;
    Report r2;
    REQUIRE(run_sync(src, dst, del, r2).ok());
    CHECK(r2.stats.failures == 0);
    CHECK(r2.stats.deleted == 1);
    CHECK(fs::is_directory(tmp.sub("dst/a.txt")));
    compare_trees(src, dst);
  }
}

TEST_CASE("FS to FS: dry run changes nothing") {
  TempDir tmp;
  make_source_tree(tmp);
  PosixEndpoint src(tmp.sub("src"));
  PosixEndpoint dst(tmp.sub("dst"));
  SyncOptions options = test_options();
  options.dry_run = true;
  Report r;
  REQUIRE(run_sync(src, dst, options, r).ok());
  CHECK(r.stats.files_copied == 4);
  CHECK(r.stats.dirs_created == 4);
  CHECK(r.stats.symlinks_created == 3);
  CHECK_FALSE(fs::exists(tmp.sub("dst")));
}

TEST_CASE("FS to FS: the adaptive controller does not delay the end of a run") {
  TempDir tmp;
  make_source_tree(tmp);
  PosixEndpoint src(tmp.sub("src"));
  PosixEndpoint dst(tmp.sub("dst"));
  SyncOptions options = test_options();
  options.adaptive = true;
  options.min_transfers = 1;
  options.transfers = 2;
  options.adapt_interval = std::chrono::seconds(60);
  Report r;
  auto start = std::chrono::steady_clock::now();
  REQUIRE(run_sync(src, dst, options, r).ok());
  CHECK(std::chrono::steady_clock::now() - start < std::chrono::seconds(10));
  CHECK(r.stats.files_copied == 4);
}

TEST_CASE("FS to FS: symlink targets can be rewritten") {
  TempDir tmp;
  make_source_tree(tmp);
  PosixEndpoint src(tmp.sub("src"));
  PosixEndpoint dst(tmp.sub("dst"));
  SyncOptions options = test_options();
  options.link_rewrites = {{"/nonexistent/", "/elsewhere/"}};
  Report r;
  REQUIRE(run_sync(src, dst, options, r).ok());
  CHECK(dst.stat("link_abs").value().link_target == "/elsewhere/abs");
  CHECK(dst.stat("link_rel").value().link_target == "a.txt");

  Report again;
  REQUIRE(run_sync(src, dst, options, again).ok());
  CHECK(again.stats.symlinks_created == 0);
  CHECK(again.stats.symlinks_unchanged == 3);
}

TEST_CASE("FS to FS: owners are preserved when running as root") {
  if (!is_root()) return;
  TempDir tmp;
  tmp.write_file("src/f", "x");
  tmp.write_file("src/d/g", "y");
  REQUIRE(symlink("f", tmp.sub("src/l").c_str()) == 0);
  REQUIRE(lchown(tmp.sub("src/f").c_str(), 1234, 1235) == 0);
  REQUIRE(lchown(tmp.sub("src/d").c_str(), 1236, 1237) == 0);
  REQUIRE(lchown(tmp.sub("src/l").c_str(), 1238, 1239) == 0);
  PosixEndpoint src(tmp.sub("src"));
  PosixEndpoint dst(tmp.sub("dst"));
  SyncOptions options = test_options();
  Report r;
  REQUIRE(run_sync(src, dst, options, r).ok());
  auto f = dst.stat("f").value();
  CHECK(f.uid == 1234);
  CHECK(f.gid == 1235);
  auto d = dst.stat("d").value();
  CHECK(d.uid == 1236);
  CHECK(d.gid == 1237);
  auto l = dst.stat("l").value();
  CHECK(l.uid == 1238);
  CHECK(l.gid == 1239);

  REQUIRE(lchown(tmp.sub("src/f").c_str(), 1300, 1301) == 0);
  Report r2;
  REQUIRE(run_sync(src, dst, options, r2).ok());
  CHECK(r2.stats.metadata_fixed == 1);
  CHECK(dst.stat("f").value().uid == 1300);
}

// ---- fake endpoint scenarios --------------------------------------------------------

namespace {

SyncOptions fake_options() {
  SyncOptions o = test_options();
  o.preserve_owner = true;
  return o;
}

void populate(FakeEndpoint& src) {
  src.add_dir("d1", 0750, {2001, 5});
  src.add_dir("d1/d2", 0700, {2002, 5});
  src.add_file("f0", "root file", 0644, {3000, 1});
  src.add_file("d1/f1", std::string(200000, 'x'), 0600, {3001, 1});
  src.add_file("d1/d2/f2", "deep", 0640, {3002, 1});
  src.add_symlink("d1/l", "f1", {4001, 0});
}

void expect_mirrored(FakeEndpoint& src, FakeEndpoint& dst) {
  for (const RelPath& p : src.paths()) {
    INFO("entry ", p);
    auto s = src.get(p);
    auto d = dst.get(p);
    REQUIRE(s);
    if (s->entry.type == EntryType::Other) {
      CHECK_FALSE(d);
      continue;
    }
    REQUIRE(d);
    CHECK(d->entry.type == s->entry.type);
    CHECK(d->entry.mtime == s->entry.mtime);
    CHECK(d->entry.uid == s->entry.uid);
    if (s->entry.type != EntryType::Symlink) CHECK(d->entry.mode == s->entry.mode);
    CHECK(d->content == s->content);
    CHECK(d->entry.link_target == s->entry.link_target);
  }
  CHECK(dst.get("")->entry.mtime == src.get("")->entry.mtime);
}

}  // namespace

TEST_CASE("fake: directory mtimes are applied after their entries") {
  FakeEndpoint src, dst;
  populate(src);
  src.modify("", [](FakeEndpoint::Node& n) { n.entry.mtime = {2000, 7}; });
  Report r;
  REQUIRE(run_sync(src, dst, fake_options(), r).ok());
  CHECK(r.stats.failures == 0);
  expect_mirrored(src, dst);

  // Fixing a file inside an existing directory touches the directory, whose
  // mtime is restored afterwards.
  src.modify("d1/f1", [](FakeEndpoint::Node& n) {
    n.content = "changed";
    n.entry.size = 7;
    n.entry.mtime = {3500, 0};
  });
  Report r2;
  REQUIRE(run_sync(src, dst, fake_options(), r2).ok());
  CHECK(r2.stats.files_copied == 1);
  expect_mirrored(src, dst);
}

TEST_CASE("fake: checksums are verified when the target computes them") {
  FakeEndpoint src, dst;
  dst.caps.checksum = ChecksumType::Adler32;
  populate(src);
  Report r;
  REQUIRE(run_sync(src, dst, fake_options(), r).ok());
  CHECK(r.stats.failures == 0);
  expect_mirrored(src, dst);
}

TEST_CASE("fake: transient errors are retried, permanent ones reported") {
  FakeEndpoint src, dst;
  populate(src);
  src.fail("list", "d1", Error{ErrorKind::IO, "flaky"}, 2);
  src.fail("open_read", "d1/d2/f2", Error{ErrorKind::Timeout, "slow"}, 1);
  src.fail("open_read", "f0", Error{ErrorKind::Permission, "nope"});
  dst.fail("mkdir", "d1/d2", Error{ErrorKind::IO, "hiccup"});
  Report r;
  REQUIRE(run_sync(src, dst, fake_options(), r).ok());
  CHECK(r.stats.retries == 4);
  CHECK(r.stats.failures == 1);
  REQUIRE(r.failures().size() == 1);
  CHECK(r.failures()[0].path == "f0");
  CHECK(r.failures()[0].error.kind == ErrorKind::Permission);
  CHECK(r.stats.files_copied == 2);
  CHECK(dst.get("d1/d2/f2"));
  CHECK_FALSE(dst.get("f0"));

  // Giving up after the configured attempts.
  FakeEndpoint src2, dst2;
  populate(src2);
  src2.fail("open_read", "f0", Error{ErrorKind::IO, "broken"}, 100);
  Report r2;
  REQUIRE(run_sync(src2, dst2, fake_options(), r2).ok());
  CHECK(r2.stats.retries == 2);
  CHECK(r2.stats.failures == 1);
}

TEST_CASE("fake: a file that changes during the copy is not committed") {
  FakeEndpoint src, dst;
  populate(src);
  int reads = 0;
  src.hook = [&](std::string_view op, const RelPath& path) {
    if (op == "read" && path == "d1/f1" && reads++ == 0)
      src.modify("d1/f1", [](FakeEndpoint::Node& n) { n.entry.mtime = {3999, 0}; });
  };
  Report r;
  REQUIRE(run_sync(src, dst, fake_options(), r).ok());
  CHECK(r.stats.retries == 1);  // the second attempt sees the new mtime and succeeds
  CHECK(r.stats.failures == 0);
  CHECK(dst.get("d1/f1")->entry.mtime == Timespec{3999, 0});

  // A file that keeps changing ends up as a failure, with nothing committed.
  FakeEndpoint src2, dst2;
  populate(src2);
  int n = 0;
  src2.hook = [&](std::string_view op, const RelPath& path) {
    if (op == "read" && path == "f0")
      src2.modify("f0", [&](FakeEndpoint::Node& x) { x.entry.mtime = {5000 + n++, 0}; });
  };
  Report r2;
  REQUIRE(run_sync(src2, dst2, fake_options(), r2).ok());
  CHECK(r2.stats.failures == 1);
  CHECK(r2.failures()[0].error.kind == ErrorKind::Changed);
  CHECK_FALSE(dst2.get("f0"));
}

TEST_CASE("fake: a corrupted transfer fails the checksum and is retried") {
  FakeEndpoint src, dst;
  dst.caps.checksum = ChecksumType::Adler32;
  populate(src);
  int writes = 0;
  dst.hook = [&](std::string_view op, const RelPath& path) {
    if (op == "commit" && path == "f0" && writes++ == 0)
      dst.fail("commit", "f0", Error{ErrorKind::Checksum, "bad checksum"});
  };
  Report r;
  REQUIRE(run_sync(src, dst, fake_options(), r).ok());
  CHECK(r.stats.retries == 1);
  CHECK(r.stats.failures == 0);
  expect_mirrored(src, dst);
}

TEST_CASE("fake: the backlog stays bounded and the walk completes with many entries") {
  FakeEndpoint src, dst;
  for (int d = 0; d < 20; ++d) {
    std::string dir = "dir" + std::to_string(d);
    src.add_dir(dir);
    for (int f = 0; f < 50; ++f) src.add_file(dir + "/f" + std::to_string(f), "content");
    src.add_dir(dir + "/sub");
    src.add_file(dir + "/sub/g", "nested");
  }
  SyncOptions o = fake_options();
  o.max_backlog = 3;
  o.checkers = 4;
  o.transfers = 3;
  Report r;
  REQUIRE(run_sync(src, dst, o, r).ok());
  CHECK(r.stats.files_copied == 20 * 51);
  CHECK(r.stats.dirs_created == 40);
  CHECK(r.stats.failures == 0);
  expect_mirrored(src, dst);
}

TEST_CASE("fake: mtimes are compared at the coarser resolution") {
  FakeEndpoint src, dst;
  dst.caps.mtime_resolution = 1000000000;
  src.add_file("f", "data", 0644, {3000, 123456789});
  Report r;
  REQUIRE(run_sync(src, dst, fake_options(), r).ok());
  // The target stored only whole seconds.
  dst.modify("f", [](FakeEndpoint::Node& n) { n.entry.mtime = {3000, 0}; });
  Report r2;
  REQUIRE(run_sync(src, dst, fake_options(), r2).ok());
  CHECK(r2.stats.files_copied == 0);
  CHECK(r2.stats.files_unchanged == 1);
}

TEST_CASE("fake: the preflight refuses to drop owners silently") {
  FakeEndpoint src, dst;
  dst.caps.can_set_owner = false;
  populate(src);
  Report r;
  Status s = run_sync(src, dst, fake_options(), r);
  REQUIRE_FALSE(s.ok());
  CHECK(s.error().kind == ErrorKind::Permission);

  SyncOptions o = fake_options();
  o.preserve_owner = false;
  Report r2;
  REQUIRE(run_sync(src, dst, o, r2).ok());
  CHECK(r2.stats.files_copied == 3);
}

TEST_CASE("fake: shards partition the files and together cover everything") {
  FakeEndpoint src;
  populate(src);
  for (int i = 0; i < 10; ++i) {
    src.add_dir("s" + std::to_string(i));
    src.add_file("s" + std::to_string(i) + "/f", "x");
  }
  const int shards = 3;
  std::vector<std::unique_ptr<FakeEndpoint>> targets;
  uint64_t copied = 0;
  for (int k = 0; k < shards; ++k) {
    targets.push_back(std::make_unique<FakeEndpoint>());
    SyncOptions o = fake_options();
    o.shard_index = k;
    o.shard_count = shards;
    Report r;
    REQUIRE(run_sync(src, *targets.back(), o, r).ok());
    CHECK(r.stats.failures == 0);
    copied += r.stats.files_copied;
  }
  CHECK(copied == src.paths().size() - 13);  // 12 directories and one symlink

  // Running all shards against one target gives the full mirror.
  FakeEndpoint dst;
  for (int k = 0; k < shards; ++k) {
    SyncOptions o = fake_options();
    o.shard_index = k;
    o.shard_count = shards;
    Report r;
    REQUIRE(run_sync(src, dst, o, r).ok());
  }
  Report check;
  REQUIRE(run_sync(src, dst, fake_options(), check).ok());
  CHECK(check.stats.files_copied == 0);
  CHECK(check.stats.symlinks_created == 0);
}

TEST_CASE("fake: cancellation stops the run and leaves nothing partial") {
  FakeEndpoint src, dst;
  for (int f = 0; f < 200; ++f) src.add_file("f" + std::to_string(f), "content");
  Cancellation cancel;
  std::atomic<int> commits{0};
  dst.hook = [&](std::string_view op, const RelPath&) {
    if (op == "commit" && ++commits == 5) cancel.request();
  };
  Report r;
  SyncOptions o = fake_options();
  o.transfers = 1;
  Engine engine(src, dst, o, r, nullptr, cancel);
  Status s = engine.run();
  REQUIRE_FALSE(s.ok());
  CHECK(s.error().kind == ErrorKind::Cancelled);
  CHECK(r.stats.files_copied < 200);
  CHECK(r.stats.failures == 0);
  for (const RelPath& p : dst.paths()) CHECK(dst.get(p)->content == "content");
}

TEST_CASE("fake: journal records failures and finalized directories, resume and retry use them") {
  TempDir tmp;
  FakeEndpoint src, dst;
  populate(src);
  src.fail("open_read", "d1/d2/f2", Error{ErrorKind::Permission, "denied"}, 100);
  src.fail("list", "d1", Error{ErrorKind::Permission, "denied"}, 1);

  auto journal = Journal::open(tmp.sub("journal.sqlite"), "fake", "fake");
  REQUIRE(journal.ok());
  Journal& j = *journal.value();
  REQUIRE(j.begin_run(false).ok());
  Report r;
  REQUIRE(run_sync(src, dst, fake_options(), r, &j).ok());
  REQUIRE(j.end_run(false).ok());  // as if interrupted
  CHECK(r.stats.failures == 1);  // the listing of d1
  auto failures = j.failures();
  REQUIRE(failures.size() == 1);
  CHECK(failures[0].path == "d1");
  CHECK(failures[0].type == EntryType::Directory);
  CHECK(j.is_done_dir(""));
  CHECK_FALSE(j.is_done_dir("d1"));

  SUBCASE("retrying the failures processes only them") {
    REQUIRE(j.begin_run(false).ok());
    Report r2;
    Cancellation cancel;
    Engine engine(src, dst, fake_options(), r2, &j, cancel);
    REQUIRE(engine.run(j.failures()).ok());
    REQUIRE(j.end_run(true).ok());
    CHECK(r2.stats.files_copied == 1);  // d1/f1; f0 was not looked at
    CHECK(r2.stats.files_checked == 2);
    CHECK(r2.stats.failures == 1);      // d1/d2/f2 still fails
    failures = j.failures();
    REQUIRE(failures.size() == 1);
    CHECK(failures[0].path == "d1/d2/f2");
    CHECK(dst.get("d1/d2"));
    CHECK(dst.get("d1/l"));

    // Once it works, the failure is cleared and nothing else is touched.
    src.fail("open_read", "d1/d2/f2", Error{ErrorKind::IO, "reset"}, 0);
    FakeEndpoint fresh_src;
    populate(fresh_src);
    REQUIRE(j.begin_run(false).ok());
    Report r3;
    Engine engine3(fresh_src, dst, fake_options(), r3, &j, cancel);
    REQUIRE(engine3.run(j.failures()).ok());
    CHECK(r3.stats.files_copied == 1);
    CHECK(r3.stats.files_checked == 1);
    CHECK(j.failures().empty());
    expect_mirrored(fresh_src, dst);
  }

  SUBCASE("resuming an interrupted run skips finalized directories") {
    int lists_before = src.count("list");
    SyncOptions o = fake_options();
    o.resume = true;
    REQUIRE(j.begin_run(true).ok());
    Report r2;
    REQUIRE(run_sync(src, dst, o, r2, &j).ok());
    CHECK(src.count("list") == lists_before);  // the root was done, nothing listed
    CHECK(r2.stats.dirs_listed == 0);
    REQUIRE(j.end_run(true).ok());

    // After a completed run there is nothing to resume: everything is walked.
    REQUIRE(j.begin_run(true).ok());
    Report r3;
    REQUIRE(run_sync(src, dst, o, r3, &j).ok());
    CHECK(r3.stats.dirs_listed == 3);
  }

  SUBCASE("a dry run leaves the journal alone") {
    SyncOptions o = fake_options();
    o.dry_run = true;
    src.fail("open_read", "f0", Error{ErrorKind::Permission, "denied"}, 100);
    Report r2;
    Cancellation cancel;
    Engine engine(src, dst, o, r2, &j, cancel);
    REQUIRE(engine.run(j.failures()).ok());
    CHECK(j.failures().size() == 1);
    CHECK(j.failures()[0].path == "d1");
  }

  SUBCASE("a journal refuses another source/target pair") {
    auto other = Journal::open(tmp.sub("journal.sqlite"), "fake", "elsewhere");
    REQUIRE_FALSE(other.ok());
  }
}

TEST_CASE("fake: journal rows are cleared by deletions, metadata fixes and vanished sources") {
  TempDir tmp;
  auto journal = Journal::open(tmp.sub("j.sqlite"), "fake", "fake");
  REQUIRE(journal.ok());
  Journal& j = *journal.value();
  Cancellation cancel;

  FakeEndpoint src, dst;
  populate(src);
  dst.add_file("extra", "x");
  dst.add_file("gone", "y");
  SyncOptions o = fake_options();
  o.delete_extra = true;
  dst.fail("remove", "extra", Error{ErrorKind::Permission, "locked"}, 1);
  REQUIRE(j.begin_run(false).ok());
  Report r;
  REQUIRE(run_sync(src, dst, o, r, &j).ok());
  REQUIRE(j.end_run(true).ok());
  CHECK(r.stats.deleted == 1);  // "gone"; the failed removal is not counted
  CHECK(r.stats.failures == 1);
  REQUIRE(j.failures().size() == 1);
  CHECK(j.failures()[0].path == "extra");

  // A metadata-only fix of a copied file: the owner differs after the copy.
  dst.modify("d1/f1", [](FakeEndpoint::Node& n) { n.entry.uid = 7; });
  dst.fail("set_metadata", "d1/f1", Error{ErrorKind::Permission, "locked"}, 1);
  REQUIRE(j.begin_run(false).ok());
  Report r2;
  REQUIRE(run_sync(src, dst, o, r2, &j).ok());
  REQUIRE(j.end_run(true).ok());
  CHECK(r2.stats.deleted == 1);  // "extra" now
  CHECK(r2.stats.failures == 1);
  REQUIRE(j.failures().size() == 1);
  CHECK(j.failures()[0].path == "d1/f1");

  REQUIRE(j.begin_run(false).ok());
  Report r3;
  REQUIRE(run_sync(src, dst, o, r3, &j).ok());
  REQUIRE(j.end_run(true).ok());
  CHECK(r3.stats.metadata_fixed == 1);
  CHECK(j.failures().empty());

  // A failure whose source vanishes is dropped when retried.
  src.fail("open_read", "f0", Error{ErrorKind::Permission, "denied"}, 1);
  src.modify("f0", [](FakeEndpoint::Node& n) { n.entry.mtime = {3333, 0}; });
  REQUIRE(j.begin_run(false).ok());
  Report r4;
  REQUIRE(run_sync(src, dst, o, r4, &j).ok());
  REQUIRE(j.end_run(true).ok());
  REQUIRE(j.failures().size() == 1);
  dst.modify("f0", [](FakeEndpoint::Node&) {});
  FakeEndpoint src2;
  populate(src2);
  src2.add_dir("other");
  REQUIRE(j.begin_run(false).ok());
  // Remove f0 from the source and from the target.
  FakeEndpoint src3, dst3;
  populate(src3);
  REQUIRE(src3.remove("f0", EntryType::File).ok());
  for (const RelPath& p : dst.paths()) {
    auto n = dst.get(p);
    if (n->entry.type == EntryType::File) dst3.add_file(p, n->content, n->entry.mode, n->entry.mtime);
  }
  Report r5;
  Engine engine(src3, dst, o, r5, &j, cancel);
  REQUIRE(dst.remove("f0", EntryType::File).ok());
  REQUIRE(engine.run(j.failures()).ok());
  CHECK(j.failures().empty());
}

TEST_CASE("fake: an exhausted deletion cap still reports blocked conflicts") {
  FakeEndpoint src, dst;
  src.add_file("conflict", "file");
  dst.add_dir("conflict");
  dst.add_file("conflict/inner", "x");
  dst.add_file("extra1", "x");
  dst.add_file("extra2", "x");
  SyncOptions o = fake_options();
  o.delete_extra = true;
  o.max_delete = 1;
  Report r;
  REQUIRE(run_sync(src, dst, o, r).ok());
  CHECK(r.stats.deleted == 1);
  bool conflict_reported = false;
  for (const Failure& f : r.failures()) conflict_reported |= f.path == "conflict";
  CHECK(conflict_reported);
  CHECK(r.stats.failures >= 1);
}

TEST_CASE("fake: a directory that cannot be created is reported once") {
  FakeEndpoint src, dst;
  populate(src);
  dst.fail("mkdir", "d1", Error{ErrorKind::Permission, "denied"}, 100);
  Report r;
  REQUIRE(run_sync(src, dst, fake_options(), r).ok());
  CHECK(r.stats.failures == 1);
  CHECK(r.failures()[0].path == "d1");
  CHECK(r.stats.files_copied == 1);  // f0

  // Also in retry mode, where the group's directory itself is missing.
  TempDir tmp;
  auto journal = Journal::open(tmp.sub("j.sqlite"), "fake", "fake");
  REQUIRE(journal.ok());
  Cancellation cancel;
  Report r2;
  Engine engine(src, dst, fake_options(), r2, journal.value().get(), cancel);
  REQUIRE(engine.run({Failure{"d1/f1", EntryType::File, Error{}}}).ok());
  CHECK(r2.stats.failures == 1);
}

TEST_CASE("fake: retry rows under a failed directory are handled once") {
  FakeEndpoint src, dst;
  populate(src);
  TempDir tmp;
  auto journal = Journal::open(tmp.sub("j.sqlite"), "fake", "fake");
  REQUIRE(journal.ok());
  Cancellation cancel;
  Report r;
  Engine engine(src, dst, fake_options(), r, journal.value().get(), cancel);
  std::vector<Failure> rows = {Failure{"d1", EntryType::Directory, Error{}},
                               Failure{"d1/d2/f2", EntryType::File, Error{}}};
  REQUIRE(engine.run(rows).ok());
  CHECK(r.stats.files_copied == 2);  // d1/f1 and d1/d2/f2, each once
  CHECK(r.stats.files_checked == 2);
}

TEST_CASE("fake: targets without mtimes compare sizes, targets without symlinks skip them") {
  FakeEndpoint src, dst;
  dst.caps.can_set_mtime = false;
  dst.caps.has_symlinks = false;
  populate(src);
  Report r;
  REQUIRE(run_sync(src, dst, fake_options(), r).ok());
  CHECK(r.stats.files_copied == 3);
  CHECK(r.stats.symlinks_skipped == 1);
  CHECK(r.stats.symlinks_created == 0);
  CHECK(r.stats.failures == 0);
  CHECK_FALSE(dst.get("d1/l"));

  src.modify("f0", [](FakeEndpoint::Node& n) { n.entry.mtime = {9999, 0}; });  // same size
  Report r2;
  REQUIRE(run_sync(src, dst, fake_options(), r2).ok());
  CHECK(r2.stats.files_copied == 0);
  CHECK(r2.stats.files_unchanged == 3);
  CHECK(r2.stats.metadata_fixed == 0);
}

TEST_CASE("fake: owners of a source without owners are left alone") {
  FakeEndpoint src, dst;
  src.caps.has_owners = false;
  dst.caps.can_set_owner = false;  // no reason to refuse the run
  populate(src);
  for (const RelPath& p : src.paths()) src.modify(p, [](FakeEndpoint::Node& n) { n.entry.uid = 0; });
  Report r;
  StderrCapture err;
  REQUIRE(run_sync(src, dst, fake_options(), r).ok());
  CHECK(err.text().find("reports no owners") != std::string::npos);
  CHECK(r.stats.failures == 0);
  CHECK(r.stats.files_copied == 3);
  for (const RelPath& p : dst.paths()) {
    INFO("entry ", p);
    CHECK(dst.get(p)->entry.uid == 1000);
  }
  Report r2;
  REQUIRE(run_sync(src, dst, fake_options(), r2).ok());
  CHECK(r2.stats.metadata_fixed == 0);
}

TEST_CASE("fake: directories that lead back to an ancestor are failures, not walked") {
  FakeEndpoint src, dst;
  src.modify("", [](FakeEndpoint::Node& n) { n.entry.id = "top"; });
  src.add_dir("a").id = "a";
  src.add_dir("a/b").id = "b";
  src.add_file("a/b/f", "content");
  src.add_dir("a/b/up").id = "a";     // like a symlink to ..
  src.add_dir("a/b/top").id = "top";  // like a symlink to the top
  src.add_dir("a/b/c").id = "c";
  src.add_dir("a/b/c/elsewhere").id = "b2";
  Report r;
  REQUIRE(run_sync(src, dst, fake_options(), r).ok());
  CHECK(r.stats.failures == 2);
  for (const Failure& f : r.failures()) {
    CHECK((f.path == "a/b/up" || f.path == "a/b/top"));
    CHECK(f.error.message.find("directory cycle") != std::string::npos);
  }
  CHECK(dst.get("a/b/f"));
  CHECK(dst.get("a/b/c/elsewhere"));
  CHECK_FALSE(dst.get("a/b/up"));
  CHECK_FALSE(dst.get("a/b/top"));
}

TEST_CASE("fake: mode bits a target does not store are not compared") {
  FakeEndpoint src, dst;
  dst.caps.file_mode_bits = 0777;
  dst.caps.dir_mode_bits = 03777;
  src.add_dir("d", 06755);
  src.add_file("d/f", "x", 04755);
  Report r;
  REQUIRE(run_sync(src, dst, fake_options(), r).ok());
  // What a target like EOS keeps of them.
  dst.modify("d", [](FakeEndpoint::Node& n) { n.entry.mode = 02755; });
  dst.modify("d/f", [](FakeEndpoint::Node& n) { n.entry.mode = 0755; });
  Report r2;
  REQUIRE(run_sync(src, dst, fake_options(), r2).ok());
  CHECK(r2.stats.metadata_fixed == 0);

  dst.modify("d", [](FakeEndpoint::Node& n) { n.entry.mode = 0755; });  // setgid lost
  Report r3;
  REQUIRE(run_sync(src, dst, fake_options(), r3).ok());
  CHECK(r3.stats.metadata_fixed == 1);
  CHECK(dst.get("d")->entry.mode == 06755);
}

TEST_CASE("fake: an engine runs only once") {
  FakeEndpoint src, dst;
  Cancellation cancel;
  Report r;
  Engine engine(src, dst, fake_options(), r, nullptr, cancel);
  REQUIRE(engine.run().ok());
  CHECK_FALSE(engine.run().ok());
}

TEST_CASE("FS to FS: long names, symlinked roots and setuid bits") {
  TempDir tmp;
  std::string long_name(250, 'n');
  tmp.write_file("src/" + long_name, "long");
  tmp.write_file("src/plain", "p");
  REQUIRE(symlink("src", tmp.sub("srclink").c_str()) == 0);
  PosixEndpoint src(tmp.sub("srclink"));
  PosixEndpoint dst(tmp.sub("dst"));
  SyncOptions options = test_options();
  Report r;
  REQUIRE(run_sync(src, dst, options, r).ok());
  CHECK(r.stats.failures == 0);
  CHECK(r.stats.files_copied == 2);
  CHECK(tmp.read_file("dst/" + long_name) == "long");
  CHECK(src.describe() == tmp.sub("src"));  // canonical

  if (is_root()) {
    // chown clears setuid bits, so the mode is set after the owner.
    REQUIRE(lchown(tmp.sub("src/plain").c_str(), 4321, 4321) == 0);
    REQUIRE(chmod(tmp.sub("src/plain").c_str(), 04755) == 0);
    Report r2;
    REQUIRE(run_sync(src, dst, options, r2).ok());
    auto st = dst.stat("plain").value();
    CHECK(st.uid == 4321);
    CHECK(st.mode == 04755);
    // Changing only the owner must still restore the setuid bit on the target.
    REQUIRE(lchown(tmp.sub("src/plain").c_str(), 4322, 4322) == 0);
    REQUIRE(chmod(tmp.sub("src/plain").c_str(), 04755) == 0);
    Report r3;
    REQUIRE(run_sync(src, dst, options, r3).ok());
    st = dst.stat("plain").value();
    CHECK(st.uid == 4322);
    CHECK(st.mode == 04755);
  }
}

TEST_CASE("FS to FS: --no-mode gives copies the default mode") {
  TempDir tmp;
  tmp.write_file("src/f", "x", 0600);
  PosixEndpoint src(tmp.sub("src"));
  PosixEndpoint dst(tmp.sub("dst"));
  SyncOptions options = test_options();
  options.preserve_mode = false;
  Report r;
  REQUIRE(run_sync(src, dst, options, r).ok());
  mode_t mask = umask(0);
  umask(mask);
  CHECK(dst.stat("f").value().mode == (0666 & ~static_cast<ModeBits>(mask)));
}

TEST_CASE("fake: copies without checksum verification are counted or refused") {
  FakeEndpoint src, dst;
  populate(src);
  Report r;
  REQUIRE(run_sync(src, dst, fake_options(), r).ok());
  CHECK(r.stats.files_copied == 3);
  CHECK(r.stats.files_unverified == 3);  // the fake computes none by default

  FakeEndpoint dst2;
  SyncOptions strict = fake_options();
  strict.require_checksum = true;
  Report r2;
  REQUIRE(run_sync(src, dst2, strict, r2).ok());
  CHECK(r2.stats.files_copied == 0);
  CHECK(r2.stats.failures == 3);
  CHECK(r2.failures()[0].error.kind == ErrorKind::Unsupported);

  FakeEndpoint dst3;
  dst3.caps.checksum = ChecksumType::Adler32;
  Report r3;
  REQUIRE(run_sync(src, dst3, strict, r3).ok());
  CHECK(r3.stats.files_copied == 3);
  CHECK(r3.stats.files_unverified == 0);
  CHECK(r3.stats.failures == 0);
}

TEST_CASE("FS to FS: --require-checksum reads local copies back") {
  TempDir tmp;
  tmp.write_file("src/f", "verify me");
  PosixEndpoint src(tmp.sub("src"));
  PosixEndpoint dst(tmp.sub("dst"));
  SyncOptions strict = test_options();
  strict.require_checksum = true;
  Report r;
  REQUIRE(run_sync(src, dst, strict, r).ok());
  CHECK(r.stats.files_copied == 1);
  CHECK(r.stats.files_unverified == 0);
  CHECK(tmp.read_file("dst/f") == "verify me");
}

TEST_CASE("fake: temporaries of the source are not entries of the tree") {
  FakeEndpoint src, dst;
  populate(src);
  src.add_file(".tmp-upload", "half written");
  src.add_file("d1/.tmp-other", "half written");
  Report r;
  REQUIRE(run_sync(src, dst, fake_options(), r).ok());
  CHECK(r.stats.files_copied == 3);
  CHECK(r.stats.files_checked == 3);
  CHECK(r.stats.failures == 0);
  CHECK_FALSE(dst.get(".tmp-upload"));
  CHECK_FALSE(dst.get("d1/.tmp-other"));
}

TEST_CASE("fake: read-only directories get their mtime before their mode") {
  FakeEndpoint src, dst;
  dst.utimes_needs_write = true;
  src.add_dir("ro", 0555, {2345, 6});
  src.add_file("ro/f", "x", 0444, {3456, 7});
  Report r;
  REQUIRE(run_sync(src, dst, fake_options(), r).ok());
  CHECK(r.stats.failures == 0);
  CHECK(dst.get("ro")->entry.mode == 0555);
  CHECK(dst.get("ro")->entry.mtime == Timespec{2345, 6});

  // Fixing the mtime of an existing read-only directory still works, since
  // the fake applies the fields in the order given.
  src.modify("ro", [](FakeEndpoint::Node& n) { n.entry.mtime = {2346, 0}; });
  dst.modify("ro", [](FakeEndpoint::Node& n) { n.entry.mode = 0755; });
  Report r2;
  REQUIRE(run_sync(src, dst, fake_options(), r2).ok());
  CHECK(r2.stats.failures == 0);
  CHECK(dst.get("ro")->entry.mtime == Timespec{2346, 0});
  CHECK(dst.get("ro")->entry.mode == 0555);
}

TEST_CASE("fake: adaptive concurrency starts low, grows and completes the tree") {
  FakeEndpoint src, dst;
  for (int d = 0; d < 10; ++d) {
    src.add_dir("d" + std::to_string(d));
    for (int f = 0; f < 40; ++f) src.add_file("d" + std::to_string(d) + "/f" + std::to_string(f), std::string(5000, 'x'));
  }
  SyncOptions o = fake_options();
  o.adaptive = true;
  o.transfers = 6;
  o.min_transfers = 2;
  o.adapt_interval = std::chrono::seconds(0);  // adjust as fast as the controller can
  o.retry.initial_delay = std::chrono::milliseconds(0);
  Report r;
  REQUIRE(run_sync(src, dst, o, r).ok());
  CHECK(r.stats.files_copied == 400);
  CHECK(r.stats.failures == 0);
  CHECK(r.stats.transfer_limit.load() >= 2);
  CHECK(r.stats.transfer_limit.load() <= 6);
  expect_mirrored(src, dst);
}

TEST_CASE("fake: listed names that cannot be joined into paths are skipped and counted") {
  FakeEndpoint src, dst;
  populate(src);
  // A broken or hostile server listing names like these would otherwise
  // make the engine write outside the directory or replace the root.
  const std::vector<std::string> bad = {"", ".", "..", "x/../../escape", std::string("nul\0", 4),
                                        "line\nbreak", "cr\r"};
  for (size_t i = 0; i < bad.size(); ++i) {
    std::string path = "bad" + std::to_string(i);
    src.add_file(path, "evil");
    src.modify(path, [&](FakeEndpoint::Node& n) { n.entry.name = bad[i]; });
  }
  Report r;
  REQUIRE(run_sync(src, dst, fake_options(), r).ok());
  CHECK(r.stats.invalid_names == bad.size());
  CHECK(r.stats.failures == 0);
  CHECK(r.stats.files_copied == 3);
  CHECK(dst.get("")->entry.type == EntryType::Directory);
  CHECK(dst.paths() == std::vector<RelPath>{"d1", "d1/d2", "d1/d2/f2", "d1/f1", "d1/l", "f0"});
  CHECK(r.summary(false).find("entries with invalid names skipped: 7") != std::string::npos);

  // On the target, such entries are neither deleted nor descended into.
  dst.add_file("junk", "x");
  dst.modify("junk", [](FakeEndpoint::Node& n) { n.entry.name = ".."; });
  dst.add_dir("extra");
  dst.add_file("extra/inner", "x");
  dst.modify("extra/inner", [](FakeEndpoint::Node& n) { n.entry.name = "../../d1"; });
  SyncOptions o = fake_options();
  o.delete_extra = true;
  Report r2;
  REQUIRE(run_sync(src, dst, o, r2).ok());
  CHECK(r2.stats.invalid_names == bad.size() + 2);
  CHECK(r2.stats.deleted == 0);
  REQUIRE(r2.failures().size() == 1);
  CHECK(r2.failures()[0].path == "extra");
  CHECK(r2.failures()[0].error.kind == ErrorKind::NotEmpty);
  CHECK(dst.get("junk"));
  CHECK(dst.get("extra/inner"));
  CHECK(dst.get("d1"));
}

TEST_CASE("fake: slow source reads are logged") {
  FakeEndpoint src, dst;
  src.add_file("f", std::string(10000, 'x'));
  src.hook = [](std::string_view op, const RelPath&) {
    if (op == "read") std::this_thread::sleep_for(std::chrono::milliseconds(5));
  };
  BufferPool pool(4096);
  Cancellation cancel;
  CopyOptions options;
  Entry listed = src.stat("f").value();
  {
    StderrCapture capture;
    REQUIRE(copy_file(src, dst, "f", listed, options, pool, cancel).ok());
    CHECK(capture.text().find("slow source read") == std::string::npos);
  }
  options.slow_read = std::chrono::milliseconds(1);
  StderrCapture capture;
  REQUIRE(copy_file(src, dst, "f", listed, options, pool, cancel).ok());
  std::string out = capture.text();
  CHECK(out.find("slow source read: f at offset 0, 4096 bytes, ") != std::string::npos);
  CHECK(out.find("slow source read: f at offset 8192, 1808 bytes, ") != std::string::npos);
}

TEST_CASE("fake: nothing is created on the target before the first chunk was read") {
  FakeEndpoint src, dst;
  populate(src);
  src.fail("read", "f0", Error{ErrorKind::Permission, "unreadable"});
  Report r;
  REQUIRE(run_sync(src, dst, fake_options(), r).ok());
  REQUIRE(r.stats.failures == 1);
  CHECK(r.failures()[0].path == "f0");
  CHECK(dst.count("open_write") == 2);  // the other files
  CHECK_FALSE(dst.get("f0"));

  // Nor is a temporary left on a local target.
  TempDir tmp;
  PosixEndpoint posix(tmp.path());
  src.fail("read", "f0", Error{ErrorKind::Permission, "unreadable"});
  Report r2;
  REQUIRE(run_sync(src, posix, test_options(), r2).ok());
  CHECK(r2.stats.failures == 1);
  CHECK(r2.stats.files_copied == 2);
  for (const auto& entry : fs::recursive_directory_iterator(tmp.path()))
    CHECK_FALSE(is_temporary_name(entry.path().filename().string()));
  CHECK_FALSE(fs::exists(tmp.sub("f0")));
}

TEST_CASE("fake: files of many chunks are written in order and counted once") {
  FakeEndpoint src, dst;
  std::string content = pattern(10 * 4096 + 17);
  src.add_file("big", content);
  src.add_file("exact", pattern(2 * 4096));
  SyncOptions o = fake_options();
  o.buffer_size = 4096;
  std::atomic<uint64_t> chunks{0};
  dst.hook = [&](std::string_view op, const RelPath&) {
    if (op == "write") ++chunks;
  };
  Report r;
  REQUIRE(run_sync(src, dst, o, r).ok());
  CHECK(r.stats.failures == 0);
  CHECK(r.stats.bytes_copied == content.size() + 2 * 4096);
  CHECK(r.stats.bytes_written == content.size() + 2 * 4096);
  CHECK(chunks == 11 + 2);
  CHECK(dst.get("big")->content == content);  // the fake refuses gaps and rewrites
  CHECK(dst.get("exact")->content == pattern(2 * 4096));
}

TEST_CASE("fake: targets without checksums are verified against the source's") {
  FakeEndpoint src;
  src.caps.checksum = ChecksumType::Adler32;
  populate(src);
  // Like EOS, the source lists the checksums it stores.
  for (const RelPath& p : src.paths()) {
    src.modify(p, [](FakeEndpoint::Node& n) {
      if (n.entry.type != EntryType::File) return;
      Hasher h(ChecksumType::Adler32);
      h.update({reinterpret_cast<const std::byte*>(n.content.data()), n.content.size()});
      n.entry.checksum = h.finish();
    });
  }
  FakeEndpoint dst;
  Report r;
  REQUIRE(run_sync(src, dst, fake_options(), r).ok());
  CHECK(r.stats.files_copied == 3);
  CHECK(r.stats.files_unverified == 0);
  CHECK(r.stats.failures == 0);

  SUBCASE("a mismatch fails the copy after retries and leaves nothing") {
    src.modify("f0", [](FakeEndpoint::Node& n) { n.entry.checksum.hex = "00000002"; });
    FakeEndpoint dst2;
    Report r2;
    REQUIRE(run_sync(src, dst2, fake_options(), r2).ok());
    REQUIRE(r2.stats.failures == 1);
    CHECK(r2.failures()[0].path == "f0");
    CHECK(r2.failures()[0].error.kind == ErrorKind::Checksum);
    CHECK(r2.stats.retries == 2);
    CHECK_FALSE(dst2.get("f0"));

    TempDir tmp;
    PosixEndpoint posix(tmp.path());
    Report r3;
    REQUIRE(run_sync(src, posix, test_options(), r3).ok());
    CHECK(r3.stats.failures == 1);
    CHECK(r3.stats.files_unverified == 0);
    for (const auto& entry : fs::recursive_directory_iterator(tmp.path()))
      CHECK_FALSE(is_temporary_name(entry.path().filename().string()));
    CHECK_FALSE(fs::exists(tmp.sub("f0")));
  }

  SUBCASE("a listed checksum is compared even where the target verifies") {
    src.modify("f0", [](FakeEndpoint::Node& n) { n.entry.checksum.hex = "00000002"; });
    FakeEndpoint dst2;
    dst2.caps.checksum = ChecksumType::Adler32;
    Report r2;
    REQUIRE(run_sync(src, dst2, fake_options(), r2).ok());
    REQUIRE(r2.stats.failures == 1);
    CHECK(r2.failures()[0].error.kind == ErrorKind::Checksum);
    CHECK_FALSE(dst2.get("f0"));
    CHECK(dst2.count("commit") == 2);  // the other files
  }

  SUBCASE("a listed checksum counts only while size and mtime are those listed") {
    src.modify("f0", [](FakeEndpoint::Node& n) { n.entry.checksum.hex = "00000002"; });
    // The file is touched after its listing, before it is opened.
    bool touched = false;
    src.hook = [&](std::string_view op, const RelPath& path) {
      if (op == "open_read" && path == "f0" && !touched) {
        touched = true;
        src.modify("f0", [](FakeEndpoint::Node& n) { n.entry.mtime = {3999, 0}; });
      }
    };
    FakeEndpoint dst2;
    Report r2;
    REQUIRE(run_sync(src, dst2, fake_options(), r2).ok());
    CHECK(r2.stats.failures == 0);
    CHECK(r2.stats.files_unverified == 1);
    CHECK(dst2.get("f0")->entry.mtime == Timespec{3999, 0});
  }

  SUBCASE("--require-checksum is met by the source") {
    SyncOptions strict = fake_options();
    strict.require_checksum = true;
    FakeEndpoint dst2;
    Report r2;
    REQUIRE(run_sync(src, dst2, strict, r2).ok());
    CHECK(r2.stats.files_copied == 3);
    CHECK(r2.stats.failures == 0);

    src.modify("f0", [](FakeEndpoint::Node& n) { n.entry.checksum = {}; });
    FakeEndpoint dst3;
    Report r3;
    REQUIRE(run_sync(src, dst3, strict, r3).ok());
    REQUIRE(r3.stats.failures == 1);
    CHECK(r3.failures()[0].path == "f0");
    CHECK(r3.failures()[0].error.kind == ErrorKind::Unsupported);
  }
}

TEST_CASE("the transfer limit grows by half while throughput improves") {
  CHECK(next_transfer_limit(4, 4, 32, false, true) == 6);
  CHECK(next_transfer_limit(6, 4, 32, false, true) == 9);
  CHECK(next_transfer_limit(1, 1, 32, false, true) == 3);    // by at least 2
  CHECK(next_transfer_limit(30, 4, 32, false, true) == 32);  // up to the maximum
  CHECK(next_transfer_limit(13, 4, 32, false, false) == 13);
  // Retries take a quarter away, whatever the throughput did.
  CHECK(next_transfer_limit(32, 4, 32, true, true) == 24);
  CHECK(next_transfer_limit(3, 1, 32, true, false) == 2);  // at least 1
  CHECK(next_transfer_limit(5, 4, 32, true, false) == 4);  // down to the minimum
  // From 4 to 32 within six intervals.
  size_t limit = 4;
  int intervals = 0;
  while (limit < 32) {
    limit = next_transfer_limit(limit, 4, 32, false, true);
    ++intervals;
  }
  CHECK(intervals == 6);
}

TEST_CASE("the transfer controller grows with the throughput and backs off on target retries") {
  TransferController c(4, 32);
  CHECK(c.limit() == 4);
  CHECK(c.update(0, 0, false) == 4);  // nothing copied yet
  CHECK(c.update(100, 10, false) == 6);
  CHECK(c.update(200, 10, false) == 9);   // bytes improved
  CHECK(c.update(200, 20, false) == 13);  // files improved
  CHECK(c.update(205, 20, false) == 13);  // within 5 % of the rate at 9
  CHECK(c.update(190, 19, false) == 13);

  // Retries on the target take a quarter away. The next interval measures
  // the lower limit, which a later one then has to beat.
  CHECK(c.update(150, 15, true) == 10);
  CHECK(c.update(120, 12, false) == 10);
  CHECK(c.update(125, 12, false) == 10);
  CHECK(c.update(140, 12, false) == 15);
  CHECK(c.update(100, 10, true) == 12);
  CHECK(c.update(100, 10, true) == 9);
  for (int i = 0; i < 10; ++i) c.update(0, 0, true);
  CHECK(c.limit() == 4);  // not below the start
}

TEST_CASE("the transfer controller tries higher limits only now and then at a flat rate") {
  TransferController c(4, 1000);
  c.update(100, 10, false);
  size_t settled = c.limit();
  int raised = 0;
  for (int i = 0; i < 100; ++i) {
    size_t before = c.limit();
    if (c.update(100, 10, false) > before) ++raised;
  }
  CHECK(raised >= 2);   // the record decays
  CHECK(raised <= 20);  // but slowly
  CHECK(c.limit() > settled);

  // A record from a faster part of the tree blocks growth only for a while.
  TransferController d(4, 1000);
  d.update(1000, 100, false);
  size_t after_fast = d.limit();
  int intervals = 0;
  while (d.limit() == after_fast && intervals < 1000) {
    d.update(500, 50, false);
    ++intervals;
  }
  CHECK(d.limit() > after_fast);
  CHECK(intervals > 50);
  CHECK(intervals < 300);
}

TEST_CASE("copies report errors of the target, not of the source") {
  FakeEndpoint src, dst;
  src.add_file("f", std::string(10000, 'x'));
  src.add_file("g", "y");
  BufferPool pool(4096);
  Cancellation cancel;
  std::vector<ErrorKind> target_errors;
  CopyOptions options;
  options.on_target_error = [&](const Error& e) { target_errors.push_back(e.kind); };
  auto copy = [&](const RelPath& path) {
    return copy_file(src, dst, path, src.stat(path).value(), options, pool, cancel);
  };

  src.fail("read", "f", Error{ErrorKind::IO, "source trouble"});
  CHECK_FALSE(copy("f").ok());
  src.fail("open_read", "f", Error{ErrorKind::Timeout, "source trouble"});
  CHECK_FALSE(copy("f").ok());
  CHECK(target_errors.empty());

  dst.fail("open_write", "f", Error{ErrorKind::Timeout, "target trouble"});
  CHECK_FALSE(copy("f").ok());
  dst.fail("write", "f", Error{ErrorKind::IO, "target trouble"});
  CHECK_FALSE(copy("f").ok());
  dst.fail("commit", "g", Error{ErrorKind::NoSpace, "full"});
  CHECK_FALSE(copy("g").ok());
  CHECK(target_errors == std::vector{ErrorKind::Timeout, ErrorKind::IO, ErrorKind::NoSpace});
  CHECK(copy("f").ok());
}

TEST_CASE("fake: the transfer slots exist before the run starts") {
  FakeEndpoint src, dst;
  Cancellation cancel;
  Report r;
  SyncOptions o = fake_options();
  o.transfers = 5;
  Engine engine(src, dst, o, r, nullptr, cancel);
  CHECK(r.slots.size() == 5);
  CHECK(r.backlog_capacity == o.max_backlog);
}

TEST_CASE("fake: a directory's metadata is set without holding a transfer slot") {
  FakeEndpoint src, dst;
  src.add_dir("a");
  src.add_file("a/f", "x");
  src.add_dir("b");
  src.add_file("b/g", "y");
  SyncOptions o = fake_options();
  o.transfers = 2;
  o.adaptive = true;
  o.min_transfers = 1;  // one copy at a time
  o.adapt_interval = std::chrono::seconds(60);
  // Finalizing a directory waits for the other copy, which needs the slot.
  std::atomic<int> writes{0};
  std::atomic<bool> waited_in_vain{false};
  dst.hook = [&](std::string_view op, const RelPath& path) {
    if (op == "open_write") ++writes;
    if (op != "set_metadata" || (path != "a" && path != "b")) return;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (writes < 2 && std::chrono::steady_clock::now() < deadline)
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    if (writes < 2) waited_in_vain = true;
  };
  Report r;
  REQUIRE(run_sync(src, dst, o, r).ok());
  CHECK(r.stats.files_copied == 2);
  CHECK_FALSE(waited_in_vain);
}

TEST_CASE("fake: cancelling during a retry wait records no failure") {
  TempDir tmp;
  auto journal = Journal::open(tmp.sub("j.sqlite"), "fake", "fake");
  REQUIRE(journal.ok());
  REQUIRE(journal.value()->begin_run(false).ok());
  FakeEndpoint src, dst;
  src.add_file("f", "x");
  src.fail("open_read", "f", Error{ErrorKind::IO, "flaky"}, 100);
  SyncOptions o = fake_options();
  o.retry.initial_delay = std::chrono::seconds(30);
  Cancellation cancel;
  std::thread stopper([&] {
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    cancel.request();
  });
  Report r;
  Engine engine(src, dst, o, r, journal.value().get(), cancel);
  auto start = std::chrono::steady_clock::now();
  Status s = engine.run();
  stopper.join();
  CHECK(std::chrono::steady_clock::now() - start < std::chrono::seconds(10));
  REQUIRE_FALSE(s.ok());
  CHECK(s.error().kind == ErrorKind::Cancelled);
  CHECK(r.stats.retries == 1);
  CHECK(r.stats.failures == 0);
  CHECK(journal.value()->failures().empty());
}

TEST_CASE("fake: only what succeeded is counted") {
  FakeEndpoint src, dst;
  src.add_dir("d", 0755, {2001, 0});
  src.add_symlink("d/l", "target");
  dst.add_dir("d", 0755, {2002, 0});
  dst.fail("symlink", "d/l", Error{ErrorKind::Permission, "denied"});
  dst.fail("set_metadata", "d", Error{ErrorKind::Permission, "denied"});
  Report r;
  REQUIRE(run_sync(src, dst, fake_options(), r).ok());
  CHECK(r.stats.failures == 2);
  CHECK(r.stats.symlinks_created == 0);
  CHECK(r.stats.metadata_fixed == 0);

  Report r2;
  REQUIRE(run_sync(src, dst, fake_options(), r2).ok());
  CHECK(r2.stats.failures == 0);
  CHECK(r2.stats.symlinks_created == 1);
  CHECK(r2.stats.metadata_fixed == 1);
}

TEST_CASE("fake: a top directory whose metadata cannot be set is a failure that can be retried") {
  TempDir tmp;
  auto journal = Journal::open(tmp.sub("j.sqlite"), "fake", "fake");
  REQUIRE(journal.ok());
  Journal& j = *journal.value();
  FakeEndpoint src, dst;
  populate(src);
  dst.fail("set_metadata", "", Error{ErrorKind::Permission, "not the owner"}, 2);
  REQUIRE(j.begin_run(false).ok());
  Report r;
  REQUIRE(run_sync(src, dst, fake_options(), r, &j).ok());
  REQUIRE(j.end_run(true).ok());
  CHECK(r.stats.failures == 1);
  CHECK(r.stats.files_copied == 3);
  REQUIRE(j.failures().size() == 1);
  CHECK(j.failures()[0].path == "");

  // Retrying the row walks the whole tree.
  for (uint64_t failures : {1, 0}) {
    REQUIRE(j.begin_run(false).ok());
    Report r2;
    Cancellation cancel;
    Engine engine(src, dst, fake_options(), r2, &j, cancel);
    REQUIRE(engine.run(j.failures()).ok());
    REQUIRE(j.end_run(true).ok());
    CHECK(r2.stats.dirs_listed == 3);
    CHECK(r2.stats.failures == failures);
    CHECK(j.failures().size() == failures);
  }
  CHECK(dst.get("")->entry.mtime == src.get("")->entry.mtime);
}

TEST_CASE("fake: retrying keeps the rows of a directory that cannot be recreated") {
  TempDir tmp;
  auto journal = Journal::open(tmp.sub("j.sqlite"), "fake", "fake");
  REQUIRE(journal.ok());
  Journal& j = *journal.value();
  FakeEndpoint src, dst;
  populate(src);
  // As if d1/d2/f2 had failed and d1 then vanished from the target.
  REQUIRE(j.begin_run(false).ok());
  j.record_failure({"d1/d2/f2", EntryType::File, Error{ErrorKind::IO, "earlier"}});
  REQUIRE(j.end_run(true).ok());

  REQUIRE(j.begin_run(false).ok());
  Report r;
  Cancellation cancel;
  Engine engine(src, dst, fake_options(), r, &j, cancel);
  REQUIRE(engine.run(j.failures()).ok());
  REQUIRE(j.end_run(true).ok());
  REQUIRE(r.stats.failures == 1);
  CHECK(r.failures()[0].path == "d1/d2");
  CHECK(r.failures()[0].error.kind == ErrorKind::NotFound);
  auto rows = j.failures();
  REQUIRE(rows.size() == 2);
  CHECK(rows[0].path == "d1/d2");
  CHECK(rows[1].path == "d1/d2/f2");

  // A full run recreates everything and clears both.
  REQUIRE(j.begin_run(false).ok());
  Report r2;
  REQUIRE(run_sync(src, dst, fake_options(), r2, &j).ok());
  REQUIRE(j.end_run(true).ok());
  CHECK(r2.stats.failures == 0);
  CHECK(j.failures().empty());
  expect_mirrored(src, dst);
}

TEST_CASE("fake: retrying a failed deletion deletes, with --delete") {
  TempDir tmp;
  auto journal = Journal::open(tmp.sub("j.sqlite"), "fake", "fake");
  REQUIRE(journal.ok());
  Journal& j = *journal.value();
  FakeEndpoint src, dst;
  populate(src);
  Report mirror;
  REQUIRE(run_sync(src, dst, fake_options(), mirror).ok());
  dst.add_file("d1/extra", "x");
  dst.add_dir("gone");
  dst.add_file("gone/inner", "y");
  REQUIRE(j.begin_run(false).ok());
  j.record_failure({"d1/extra", EntryType::File, Error{ErrorKind::Permission, "locked"}});
  j.record_failure({"gone", EntryType::Directory, Error{ErrorKind::Permission, "locked"}});
  j.record_failure({"gone/inner", EntryType::File, Error{ErrorKind::IO, "earlier"}});
  REQUIRE(j.end_run(true).ok());

  for (bool delete_extra : {false, true}) {
    SyncOptions o = fake_options();
    o.delete_extra = delete_extra;
    REQUIRE(j.begin_run(false).ok());
    Report r;
    Cancellation cancel;
    Engine engine(src, dst, o, r, &j, cancel);
    REQUIRE(engine.run(j.failures()).ok());
    REQUIRE(j.end_run(true).ok());
    CHECK(r.stats.failures == 0);
    CHECK(r.stats.extras == 2);
    CHECK(r.stats.deleted == (delete_extra ? 3 : 0));
    CHECK(j.failures().size() == (delete_extra ? 0 : 3));
    CHECK(static_cast<bool>(dst.get("gone/inner")) == !delete_extra);
  }
  CHECK_FALSE(dst.get("d1/extra"));
  CHECK(dst.get("d1/f1"));
}

TEST_CASE("fake: plain runs clear the rows of entries gone from both sides") {
  TempDir tmp;
  auto journal = Journal::open(tmp.sub("j.sqlite"), "fake", "fake");
  REQUIRE(journal.ok());
  Journal& j = *journal.value();
  FakeEndpoint src, dst;
  populate(src);
  Report mirror;
  REQUIRE(run_sync(src, dst, fake_options(), mirror).ok());
  dst.add_file("d1/extra", "x");
  REQUIRE(j.begin_run(false).ok());
  for (const char* path : {"d1/vanished", "d1/extra", "nowhere/deep/file", "d1/d2/gone/f"})
    j.record_failure({path, EntryType::File, Error{ErrorKind::IO, "earlier"}});
  j.record_failure({"nowhere", EntryType::Directory, Error{ErrorKind::IO, "earlier"}});
  REQUIRE(j.end_run(true).ok());

  REQUIRE(j.begin_run(false).ok());
  Report r;
  REQUIRE(run_sync(src, dst, fake_options(), r, &j).ok());
  REQUIRE(j.end_run(true).ok());
  // Only the extra, which a run with --delete would delete, is left.
  auto rows = j.failures();
  REQUIRE(rows.size() == 1);
  CHECK(rows[0].path == "d1/extra");
}

TEST_CASE("fake: read-only target directories are made writable for changes, and restored") {
  FakeEndpoint src, dst;
  dst.caps.can_set_owner = false;
  dst.utimes_needs_write = true;
  dst.entries_need_write = true;
  SyncOptions o = fake_options();
  o.preserve_owner = false;
  src.add_dir("ro", 0555, {2345, 6});
  src.add_file("ro/f", "x", 0444, {3456, 7});
  src.add_dir("ro/sub", 0555, {2346, 0});
  Report r;
  REQUIRE(run_sync(src, dst, o, r).ok());
  CHECK(r.stats.failures == 0);
  expect_mirrored(src, dst);

  SUBCASE("a new entry") {
    src.add_file("ro/g", "new", 0444, {3457, 0});
    src.modify("ro", [](FakeEndpoint::Node& n) { n.entry.mtime = {2350, 0}; });
  }
  SUBCASE("only the directory's mtime") {
    // Setting it needs write access as well (EOS, a non-owner on POSIX).
    src.modify("ro", [](FakeEndpoint::Node& n) { n.entry.mtime = {2350, 0}; });
  }
  SUBCASE("a replaced file in a directory that keeps its mtime") {
    src.modify("ro/f", [](FakeEndpoint::Node& n) {
      n.content = "changed";
      n.entry.size = 7;
      n.entry.mtime = {3500, 0};
    });
  }
  Report r2;
  REQUIRE(run_sync(src, dst, o, r2).ok());
  CHECK(r2.stats.failures == 0);
  expect_mirrored(src, dst);
  CHECK(dst.get("ro")->entry.mode == 0555);

  // --delete removes read-only trees.
  dst.add_dir("ro/extra", 0555);
  dst.add_dir("ro/extra/inner", 0555);
  dst.add_file("ro/extra/inner/f", "x", 0444);
  o.delete_extra = true;
  Report r3;
  REQUIRE(run_sync(src, dst, o, r3).ok());
  CHECK(r3.stats.failures == 0);
  CHECK(r3.stats.deleted == 3);
  CHECK_FALSE(dst.get("ro/extra"));
  expect_mirrored(src, dst);
}

TEST_CASE("fake: shards restore the modes of read-only directories they did not own") {
  FakeEndpoint src, dst;
  dst.caps.can_set_owner = false;
  dst.entries_need_write = true;
  const int shards = 3;
  auto run_shards = [&] {
    for (int k = 0; k < shards; ++k) {
      SyncOptions o = fake_options();
      o.preserve_owner = false;
      o.shard_index = k;
      o.shard_count = shards;
      Report r;
      REQUIRE(run_sync(src, dst, o, r).ok());
      CHECK(r.stats.failures == 0);
    }
  };
  for (int i = 0; i < 6; ++i) src.add_dir("p" + std::to_string(i), 0555);
  run_shards();
  // Every shard creates new subdirectories, also in directories that other
  // shards own.
  for (int i = 0; i < 6; ++i) src.add_dir("p" + std::to_string(i) + "/new", 0755);
  run_shards();
  for (int i = 0; i < 6; ++i) {
    INFO("p", i);
    CHECK(dst.get("p" + std::to_string(i))->entry.mode == 0555);
    CHECK(dst.get("p" + std::to_string(i) + "/new"));
  }
}

TEST_CASE("fake: an empty source does not empty the target") {
  FakeEndpoint src, dst;
  dst.add_file("precious", "x");
  dst.add_dir("tree");
  SyncOptions o = fake_options();
  o.delete_extra = true;
  Report r;
  Status s = run_sync(src, dst, o, r);
  REQUIRE_FALSE(s.ok());
  CHECK(s.error().kind != ErrorKind::Cancelled);
  CHECK(s.error().message.find("refusing to delete") != std::string::npos);
  CHECK(r.stats.deleted == 0);
  CHECK(dst.get("precious"));

  // Without --delete, or with something in the source, nothing is refused.
  o.delete_extra = false;
  Report r2;
  REQUIRE(run_sync(src, dst, o, r2).ok());
  CHECK(r2.stats.extras == 2);
  src.add_file("f", "y");
  o.delete_extra = true;
  Report r3;
  REQUIRE(run_sync(src, dst, o, r3).ok());
  CHECK(r3.stats.deleted == 2);
  CHECK(r3.stats.files_copied == 1);

  // A missing source stops the run before anything is done.
  TempDir tmp;
  tmp.write_file("dst/keep", "x");
  PosixEndpoint missing(tmp.sub("src"));
  PosixEndpoint posix_dst(tmp.sub("dst"));
  SyncOptions po = test_options();
  po.delete_extra = true;
  Report r4;
  Status gone = run_sync(missing, posix_dst, po, r4);
  REQUIRE_FALSE(gone.ok());
  CHECK(gone.error().kind == ErrorKind::NotFound);
  CHECK(fs::exists(tmp.sub("dst/keep")));
}

TEST_CASE("fake: a run stops after too many failures in a row") {
  FakeEndpoint src, dst;
  for (int i = 0; i < 40; ++i) {
    std::string name = "f" + std::to_string(10 + i);
    src.add_file(name, "x");
    dst.fail("open_write", name, Error{ErrorKind::NoSpace, "full"}, 100);
  }
  SyncOptions o = fake_options();
  o.max_consecutive_failures = 5;
  Report r;
  Status s = run_sync(src, dst, o, r);
  REQUIRE_FALSE(s.ok());
  CHECK(s.error().kind == ErrorKind::Cancelled);
  CHECK(s.error().message.find("5 failures in a row") != std::string::npos);
  CHECK(r.stats.failures >= 5);
  CHECK(r.stats.failures < 40);

  o.max_consecutive_failures = 0;  // never
  Report r2;
  REQUIRE(run_sync(src, dst, o, r2).ok());
  CHECK(r2.stats.failures == 40);

  // Successes in between start the count anew.
  FakeEndpoint src2, dst2;
  for (int i = 0; i < 40; ++i) {
    std::string name = "f" + std::to_string(10 + i);
    src2.add_file(name, "x");
    if (i % 2) dst2.fail("open_write", name, Error{ErrorKind::NoSpace, "full"}, 100);
  }
  o.max_consecutive_failures = 2;
  o.checkers = 1;
  o.transfers = 1;
  Report r3;
  REQUIRE(run_sync(src2, dst2, o, r3).ok());
  CHECK(r3.stats.failures == 20);
  CHECK(r3.stats.files_copied == 20);
}

TEST_CASE("FS to FS: a source is never written to") {
  TempDir tmp;
  make_source_tree(tmp);
  struct stat before{};
  REQUIRE(stat(tmp.sub("src").c_str(), &before) == 0);
  PosixEndpoint src(tmp.sub("src"));
  PosixEndpoint dst(tmp.sub("dst"));
  Report r;
  REQUIRE(run_sync(src, dst, test_options(), r).ok());
  struct stat after{};
  REQUIRE(stat(tmp.sub("src").c_str(), &after) == 0);
  // Creating and removing a probe file would change the ctime.
  CHECK(after.st_ctim.tv_sec == before.st_ctim.tv_sec);
  CHECK(after.st_ctim.tv_nsec == before.st_ctim.tv_nsec);
}

TEST_CASE("FS to FS: a journal recognizes a target that did not exist on the first run") {
  TempDir tmp;
  tmp.write_file("src/f", "x");
  fs::create_directories(tmp.sub("real"));
  REQUIRE(symlink("real", tmp.sub("link").c_str()) == 0);
  PosixEndpoint src(tmp.sub("src"));
  std::string journal_file = tmp.sub("j.sqlite");
  {
    PosixEndpoint dst(tmp.sub("link/./mirror/"));
    auto journal = Journal::open(journal_file, src.describe(), dst.describe());
    REQUIRE(journal.ok());
    REQUIRE(journal.value()->begin_run(false).ok());
    Report r;
    REQUIRE(run_sync(src, dst, test_options(), r, journal.value().get()).ok());
    REQUIRE(journal.value()->end_run(false).ok());
  }
  PosixEndpoint dst(tmp.sub("real/mirror"));
  CHECK(dst.describe() == fs::canonical(tmp.sub("real/mirror")).string());
  auto journal = Journal::open(journal_file, src.describe(), dst.describe());
  CHECK(journal.ok());
}

// Run as nobody by the unit_unprivileged test, see tests/CMakeLists.txt.
TEST_CASE("FS to FS as a user other than root: read-only directories" *
          doctest::test_suite("unprivileged")) {
  if (is_root()) return;
  TempDir tmp;
  tmp.write_file("src/ro/f", "x", 0444);
  tmp.write_file("src/ro/sub/g", "y", 0444);
  for (const char* dir : {"src/ro/sub", "src/ro"}) REQUIRE(chmod(tmp.sub(dir).c_str(), 0555) == 0);
  set_mtime(tmp.sub("src/ro/sub"), 1600000000, 1);
  set_mtime(tmp.sub("src/ro"), 1600000001, 2);
  PosixEndpoint src(tmp.sub("src"));
  PosixEndpoint dst(tmp.sub("dst"));
  SyncOptions options = test_options();
  REQUIRE_FALSE(options.preserve_owner);
  Report r;
  REQUIRE(run_sync(src, dst, options, r).ok());
  CHECK(r.stats.failures == 0);
  compare_trees(src, dst);

  // A new entry in one read-only directory, a new mtime of another, and a
  // read-only tree to delete in the first.
  REQUIRE(chmod(tmp.sub("src/ro").c_str(), 0755) == 0);
  tmp.write_file("src/ro/new", "z", 0444);
  REQUIRE(chmod(tmp.sub("src/ro").c_str(), 0555) == 0);
  set_mtime(tmp.sub("src/ro"), 1600000003, 4);
  set_mtime(tmp.sub("src/ro/sub"), 1600000005, 6);
  REQUIRE(chmod(tmp.sub("dst/ro").c_str(), 0755) == 0);
  tmp.write_file("dst/ro/extra/inner/f", "x", 0444);
  for (const char* dir : {"dst/ro/extra/inner", "dst/ro/extra", "dst/ro"})
    REQUIRE(chmod(tmp.sub(dir).c_str(), 0555) == 0);
  options.delete_extra = true;
  Report r2;
  REQUIRE(run_sync(src, dst, options, r2).ok());
  CHECK(r2.stats.failures == 0);
  CHECK(r2.stats.files_copied == 1);
  CHECK(r2.stats.deleted == 3);
  CHECK_FALSE(fs::exists(tmp.sub("dst/ro/extra")));
  compare_trees(src, dst);
}
