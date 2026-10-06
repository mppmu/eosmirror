// SPDX-License-Identifier: GPL-3.0-or-later
#include "eosmirror/engine.hh"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <filesystem>
#include <thread>

#include "doctest/doctest.h"
#include "eosmirror/posix_endpoint.hh"
#include "fake_endpoint.hh"
#include "temp_dir.hh"

using namespace eosmirror;
namespace fs = std::filesystem;

namespace {

SyncOptions test_options() {
  SyncOptions o;
  o.checkers = 2;
  o.transfers = 2;
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
  REQUIRE(j.end_run(true).ok());
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

  SUBCASE("resuming skips finalized directories") {
    int lists_before = src.count("list");
    SyncOptions o = fake_options();
    o.resume = true;
    REQUIRE(j.begin_run(true).ok());
    Report r2;
    REQUIRE(run_sync(src, dst, o, r2, &j).ok());
    CHECK(src.count("list") == lists_before);  // the root was done, nothing listed
    CHECK(r2.stats.dirs_listed == 0);

    // A fresh run forgets the finalized directories.
    REQUIRE(j.begin_run(false).ok());
    CHECK_FALSE(j.is_done_dir(""));
  }

  SUBCASE("a journal refuses another source/target pair") {
    auto other = Journal::open(tmp.sub("journal.sqlite"), "fake", "elsewhere");
    REQUIRE_FALSE(other.ok());
  }
}
