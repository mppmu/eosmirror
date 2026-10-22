// SPDX-License-Identifier: GPL-3.0-or-later
#include "eosmirror/engine.hh"

#include <fcntl.h>
#include <sqlite3.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <map>
#include <thread>

#include "doctest/doctest.h"
#include "eosmirror/copy.hh"
#include "eosmirror/posix_endpoint.hh"
#include "fake_endpoint.hh"
#include "no_new_threads.hh"
#include "stderr_capture.hh"
#include "temp_dir.hh"

using namespace eosmirror;
namespace fs = std::filesystem;

namespace {

SyncOptions test_options() {
  SyncOptions o;
  o.checkers = 2;
  o.min_transfers = o.max_transfers = 2;
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
  for (const Entry& e : s.value().entries) {
    if (e.type == EntryType::Other) continue;
    RelPath path = join(dir, e.name);
    INFO("entry ", path);
    auto it = std::find_if(d.value().entries.begin(), d.value().entries.end(),
                           [&](const Entry& x) { return x.name == e.name; });
    REQUIRE(it != d.value().entries.end());
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

  SUBCASE("temporaries are not extras, and recent ones stay with --delete") {
    // Copies set the mtime of their temporary before renaming it. The ctime
    // tells that it is being worked on.
    tmp.write_file("dst/.a.txt.eosmirror-0123456789ab", "partial");
    set_mtime(tmp.sub("dst/.a.txt.eosmirror-0123456789ab"), 1500000000, 0);
    tmp.write_file("dst/.fresh.eosmirror-0123456789ab", "in progress");
    SyncOptions del = options;
    del.delete_extra = true;
    Report r;
    REQUIRE(run_sync(src, dst, del, r).ok());
    CHECK(r.stats.extras == 0);
    CHECK(r.stats.stale_temps == 0);
    CHECK(r.stats.deleted == 0);
    CHECK(fs::exists(tmp.sub("dst/.a.txt.eosmirror-0123456789ab")));
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
  options.min_transfers = 1;
  options.max_transfers = 2;
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

std::string adler_of(std::string_view data) {
  Hasher h(ChecksumType::Adler32);
  h.update({reinterpret_cast<const std::byte*>(data.data()), data.size()});
  return h.finish().hex;
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

TEST_CASE("fake: a run whose threads cannot be started fails, not the process") {
  FakeEndpoint src, dst;
  populate(src);
  Report r;
  NoNewThreads limit;
  Status s = run_sync(src, dst, fake_options(), r);
  CHECK(s.ok() == !limit.binds());
  if (!s.ok()) CHECK(s.error().message == "start the threads of the run");
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

TEST_CASE("fake: an upload whose close failed is taken back where it is in place") {
  FakeEndpoint src, dst;
  dst.atomic_uploads = true;
  populate(src);
  dst.add_dir("d1");
  dst.add_dir("d1/d2");
  dst.add_file("d1/d2/f2", "older", 0640, {2500, 0});
  // EOS can commit an upload whose close fails: the file is then in place
  // with the source's size and mtime. Every attempt fails like that, and
  // d1/f1 cannot be removed either.
  const int attempts = fake_options().retry.attempts;
  for (const char* path : {"f0", "d1/f1"})
    dst.fail("close", path, Error{ErrorKind::IO, "close failed"}, attempts);
  dst.fail("remove", "d1/f1", Error{ErrorKind::Permission, "locked"}, attempts);
  // A close that fails before the commit leaves the previous file alone.
  dst.fail("commit", "d1/d2/f2", Error{ErrorKind::IO, "connection lost"}, attempts);
  Report r;
  REQUIRE(run_sync(src, dst, fake_options(), r).ok());
  CHECK(r.stats.failures == 3);
  CHECK_FALSE(dst.get("f0"));
  REQUIRE(dst.get("d1/f1"));
  CHECK(dst.get("d1/f1")->entry.mtime == Timespec{0, 0});
  CHECK(dst.get("d1/d2/f2")->content == "older");

  // So the next run copies all of them.
  Report r2;
  REQUIRE(run_sync(src, dst, fake_options(), r2).ok());
  CHECK(r2.stats.files_copied == 3);
  CHECK(r2.stats.failures == 0);
  expect_mirrored(src, dst);
}

TEST_CASE("fake: uploads that fail their verification are taken back") {
  FakeEndpoint src, dst;
  dst.atomic_uploads = true;
  dst.caps.checksum = ChecksumType::Adler32;
  populate(src);
  const int attempts = fake_options().retry.attempts;
  // The data of f0 arrives corrupted, and the upload of d1/f1 cannot be
  // confirmed, though it can be found in place again. Neither can be removed.
  int confirms = 0;
  dst.hook = [&](std::string_view op, const RelPath& path) {
    if (op == "close" && path == "f0")
      dst.modify("f0", [](FakeEndpoint::Node& n) { n.content[0] = 'R'; });
    if (op == "confirm" && path == "d1/f1" && confirms++ % 2 == 0)
      dst.fail("confirm", "d1/f1", Error{ErrorKind::IO, "no answer"});
  };
  for (const char* path : {"f0", "d1/f1"})
    dst.fail("remove", path, Error{ErrorKind::Permission, "locked"}, attempts);
  Report r;
  REQUIRE(run_sync(src, dst, fake_options(), r).ok());
  CHECK(r.stats.failures == 2);
  for (const char* path : {"f0", "d1/f1"}) CHECK(dst.get(path)->entry.mtime == Timespec{0, 0});
  dst.hook = nullptr;
  Report r2;
  REQUIRE(run_sync(src, dst, fake_options(), r2).ok());
  CHECK(r2.stats.files_copied == 2);
  expect_mirrored(src, dst);
}

TEST_CASE("fake: a file that cannot be told from a failed upload is left alone") {
  FakeEndpoint src, dst;
  dst.atomic_uploads = true;
  populate(src);
  dst.add_file("f0", "previous", 0644, {2500, 0});
  // The storage node discards the upload, the close reports success, and the
  // queries that would show the previous file in place time out.
  const int attempts = fake_options().retry.attempts;
  dst.fail("lost", "f0", Error{ErrorKind::IO, "connection gone"}, attempts);
  dst.fail("confirm", "f0", Error{ErrorKind::Timeout, "no answer"}, 2 * attempts);
  Report r;
  REQUIRE(run_sync(src, dst, fake_options(), r).ok());
  REQUIRE(r.stats.failures == 1);
  CHECK(r.failures()[0].error.kind == ErrorKind::IO);
  REQUIRE(dst.get("f0"));
  CHECK(dst.get("f0")->content == "previous");
  CHECK(dst.get("f0")->entry.mtime == Timespec{2500, 0});
}

TEST_CASE("fake: an upload is confirmed by its file, not by what is in its place") {
  FakeEndpoint src, dst;
  dst.atomic_uploads = true;
  dst.caps.checksum = ChecksumType::Adler32;
  populate(src);
  Report mirror;
  REQUIRE(run_sync(src, dst, fake_options(), mirror).ok());

  SUBCASE("a lost upload, whose file in place has its checksum") {
    // The same content, other mtime: copied, and the first upload is lost.
    dst.modify("f0", [](FakeEndpoint::Node& n) { n.entry.mtime = {2500, 0}; });
    dst.fail("lost", "f0", Error{ErrorKind::IO, "connection gone"});
    Report r;
    REQUIRE(run_sync(src, dst, fake_options(), r).ok());
    CHECK(r.stats.retries == 1);
    CHECK(r.stats.failures == 0);
    CHECK(dst.get("f0")->entry.mtime == src.get("f0")->entry.mtime);
  }
  SUBCASE("a failed upload that another writer replaced meanwhile") {
    src.modify("f0", [](FakeEndpoint::Node& n) { n.entry.mtime = {3500, 0}; });
    // The first upload arrives corrupted, and before it is taken back,
    // another run puts a good copy in its place, which stays.
    bool corrupted = false;
    int confirms = 0;
    dst.hook = [&](std::string_view op, const RelPath& path) {
      if (path != "f0") return;
      if (op == "close" && !corrupted) {
        corrupted = true;
        dst.modify("f0", [](FakeEndpoint::Node& n) { n.content[0] = 'R'; });
      }
      if (op == "confirm" && ++confirms == 2)
        dst.modify("f0", [](FakeEndpoint::Node& n) {
          n.content = "root file";
          n.entry.id = "another run's";
        });
    };
    Report r;
    REQUIRE(run_sync(src, dst, fake_options(), r).ok());
    CHECK(dst.count("remove") == 0);
    CHECK(r.stats.retries == 1);
    CHECK(r.stats.failures == 0);
    expect_mirrored(src, dst);
  }
}

TEST_CASE("fake: a run that ends before an upload is confirmed leaves it in doubt") {
  TempDir tmp;
  std::string file = tmp.sub("j.sqlite");
  auto journal = Journal::open(file, "fake", "fake");
  REQUIRE(journal.ok());
  Journal& j = *journal.value();
  FakeEndpoint src, dst;
  dst.atomic_uploads = true;
  populate(src);
  // The journal as left by a run that ends between the close of f0 and its
  // confirmation: the copy is in place, but nothing confirmed it.
  std::string ended = tmp.sub("ended.sqlite");
  dst.hook = [&](std::string_view op, const RelPath& path) {
    if (op != "confirm" || path != "f0") return;
    sqlite3* db = nullptr;
    REQUIRE(sqlite3_open(file.c_str(), &db) == SQLITE_OK);
    CHECK(sqlite3_exec(db, ("VACUUM INTO '" + ended + "'").c_str(), nullptr, nullptr, nullptr) ==
          SQLITE_OK);
    sqlite3_close(db);
  };
  // One copy at a time, so that no other is under way then.
  SyncOptions one = fake_options();
  one.min_transfers = one.max_transfers = 1;
  REQUIRE(j.begin_run(false).ok());
  Report r;
  REQUIRE(run_sync(src, dst, one, r, &j).ok());
  REQUIRE(j.end_run(true).ok());
  CHECK(r.stats.files_copied == 3);
  CHECK(j.failures().value().empty());
  dst.hook = nullptr;

  // The next run with that journal copies it again, though it has the
  // source's size and mtime.
  auto after = Journal::open(ended, "fake", "fake");
  REQUIRE(after.ok());
  REQUIRE(after.value()->begin_run(false).ok());
  Report r2;
  REQUIRE(run_sync(src, dst, fake_options(), r2, after.value().get()).ok());
  REQUIRE(after.value()->end_run(true).ok());
  CHECK(r2.stats.files_copied == 1);
  CHECK(r2.stats.files_unchanged == 2);
  CHECK(after.value()->failures().value().empty());
  expect_mirrored(src, dst);
}

TEST_CASE("fake: files whose last copy failed are copied again, whatever their size and mtime") {
  TempDir tmp;
  auto journal = Journal::open(tmp.sub("j.sqlite"), "fake", "fake");
  REQUIRE(journal.ok());
  Journal& j = *journal.value();
  FakeEndpoint src, dst;
  populate(src);
  Report mirror;
  REQUIRE(run_sync(src, dst, fake_options(), mirror).ok());
  // Copies whose verification or close failed, in place with the source's
  // size and mtime, and a file whose owner could not be set.
  dst.modify("f0", [](FakeEndpoint::Node& n) { n.content = "root fil3"; });
  dst.modify("d1/d2/f2", [](FakeEndpoint::Node& n) { n.content = "dEEp"; });
  REQUIRE(j.begin_run(false).ok());
  REQUIRE(j.record_failure({"f0", EntryType::File, Error{ErrorKind::Checksum, "mismatch"}}).ok());
  REQUIRE(j.record_failure(
              {"d1/d2/f2", EntryType::File, Error{ErrorKind::IO, "close failed"}})
              .ok());
  REQUIRE(j.record_failure({"d1/f1", EntryType::File, Error{ErrorKind::Permission, "chown"}}).ok());
  REQUIRE(j.end_run(true).ok());

  SUBCASE("retrying the failures") {
    REQUIRE(j.begin_run(false).ok());
    Report r;
    Cancellation cancel;
    Engine engine(src, dst, fake_options(), r, &j, cancel);
    REQUIRE(engine.run(j.failures().value()).ok());
    REQUIRE(j.end_run(true).ok());
    CHECK(r.stats.files_copied == 2);
    CHECK(r.stats.files_unchanged == 1);
  }
  SUBCASE("a plain run") {
    REQUIRE(j.begin_run(false).ok());
    Report r;
    REQUIRE(run_sync(src, dst, fake_options(), r, &j).ok());
    REQUIRE(j.end_run(true).ok());
    CHECK(r.stats.files_copied == 2);
    CHECK(r.stats.files_unchanged == 1);
  }
  CHECK(j.failures().value().empty());
  expect_mirrored(src, dst);
}

TEST_CASE("fake: a file in doubt is not taken for its new copy when that does not arrive") {
  TempDir tmp;
  auto journal = Journal::open(tmp.sub("j.sqlite"), "fake", "fake");
  REQUIRE(journal.ok());
  Journal& j = *journal.value();
  FakeEndpoint src, dst;
  dst.atomic_uploads = true;  // without checksums
  populate(src);
  Report mirror;
  REQUIRE(run_sync(src, dst, fake_options(), mirror).ok());
  // A wrong upload stayed in place with the source's size and mtime.
  dst.modify("f0", [](FakeEndpoint::Node& n) { n.content = "root fil3"; });
  REQUIRE(j.begin_run(false).ok());
  REQUIRE(j.record_failure({"f0", EntryType::File, Error{ErrorKind::IO, "close failed"}}).ok());
  REQUIRE(j.end_run(true).ok());

  // The close of the new copy reports success, but its upload is lost.
  dst.fail("lost", "f0", Error{ErrorKind::IO, "connection gone"});
  REQUIRE(j.begin_run(false).ok());
  Report r;
  REQUIRE(run_sync(src, dst, fake_options(), r, &j).ok());
  REQUIRE(j.end_run(true).ok());
  CHECK(r.stats.failures == 0);
  CHECK(r.stats.retries == 1);
  CHECK(dst.get("f0")->content == "root file");
  CHECK(j.failures().value().empty());
}

TEST_CASE("fake: an upload that cannot be taken back leaves its copy in doubt") {
  TempDir tmp;
  auto journal = Journal::open(tmp.sub("j.sqlite"), "fake", "fake");
  REQUIRE(journal.ok());
  Journal& j = *journal.value();
  FakeEndpoint src, dst;
  dst.atomic_uploads = true;
  populate(src);
  // No checksum to meet --require-checksum with, and f0 can be neither
  // removed nor given mtime 0 afterwards.
  SyncOptions o = fake_options();
  o.require_checksum = true;
  for (const char* op : {"remove", "set_metadata"})
    dst.fail(op, "f0", Error{ErrorKind::Permission, "locked"}, o.retry.attempts);
  REQUIRE(j.begin_run(false).ok());
  Report r;
  REQUIRE(run_sync(src, dst, o, r, &j).ok());
  REQUIRE(j.end_run(true).ok());
  CHECK(r.stats.failures == 3);
  auto rows = j.failures();
  REQUIRE(rows.ok());
  for (const Failure& f : rows.value())
    CHECK(f.error.kind == (f.path == "f0" ? ErrorKind::IO : ErrorKind::Unsupported));
  REQUIRE(dst.get("f0"));

  // So the next run copies it again, also without --require-checksum.
  o.require_checksum = false;
  REQUIRE(j.begin_run(false).ok());
  Report r2;
  REQUIRE(run_sync(src, dst, o, r2, &j).ok());
  REQUIRE(j.end_run(true).ok());
  CHECK(r2.stats.files_copied == 3);
  CHECK(r2.stats.files_unchanged == 0);
  CHECK(j.failures().value().empty());
  expect_mirrored(src, dst);
}

TEST_CASE("fake: a copy stays in doubt however its later attempts end") {
  TempDir tmp;
  auto journal = Journal::open(tmp.sub("j.sqlite"), "fake", "fake");
  REQUIRE(journal.ok());
  Journal& j = *journal.value();
  FakeEndpoint src, dst;
  dst.atomic_uploads = true;
  populate(src);
  // The first attempt's close fails after the upload is in place, which can
  // be neither removed nor given mtime 0.
  dst.fail("close", "f0", Error{ErrorKind::IO, "close failed"});
  dst.fail("remove", "f0", Error{ErrorKind::Permission, "locked"});
  dst.fail("set_metadata", "f0", Error{ErrorKind::Permission, "locked"});
  Cancellation cancel;
  SUBCASE("the next attempt fails otherwise") {
    dst.hook = [&](std::string_view op, const RelPath& path) {
      if (op == "set_metadata" && path == "f0")
        dst.fail("open_write", "f0", Error{ErrorKind::Permission, "denied"});
    };
    REQUIRE(j.begin_run(false).ok());
    Report r;
    Engine engine(src, dst, fake_options(), r, &j, cancel);
    REQUIRE(engine.run().ok());
    CHECK(r.stats.failures == 1);
  }
  SUBCASE("the run is cancelled before the next attempt") {
    dst.hook = [&](std::string_view op, const RelPath& path) {
      if (op == "set_metadata" && path == "f0") cancel.request();
    };
    REQUIRE(j.begin_run(false).ok());
    Report r;
    Engine engine(src, dst, fake_options(), r, &j, cancel);
    Status s = engine.run();
    REQUIRE_FALSE(s.ok());
    CHECK(s.error().kind == ErrorKind::Cancelled);
  }
  auto rows = j.failures().value();
  REQUIRE(rows.size() == 1);
  CHECK(rows[0].path == "f0");
  CHECK(rows[0].error.kind == ErrorKind::IO);
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
  o.min_transfers = o.max_transfers = 3;
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

TEST_CASE("fake: what a target says about writing to it is shown at the start") {
  FakeEndpoint src, dst;
  populate(src);
  dst.warning = "a FUSE mount";
  dst.note = "pipelined writes";
  StderrCapture err;
  Report r;
  REQUIRE(run_sync(src, dst, fake_options(), r).ok());
  CHECK(err.text().find("WARN  a FUSE mount") != std::string::npos);
  CHECK(err.text().find("INFO  pipelined writes") != std::string::npos);
}

TEST_CASE("fake: transfers that wait for listings are counted") {
  FakeEndpoint src, dst;
  for (int d = 0; d < 10; ++d) {
    src.add_dir("d" + std::to_string(d));
    src.add_file("d" + std::to_string(d) + "/f", "x");
  }
  src.hook = [](std::string_view op, const RelPath&) {
    if (op == "list") std::this_thread::sleep_for(std::chrono::milliseconds(20));
  };
  SyncOptions o = fake_options();
  o.checkers = 1;
  Report r;
  REQUIRE(run_sync(src, dst, o, r).ok());
  CHECK(r.stats.files_copied == 10);
  CHECK(r.stats.listing_bound > 0);
  CHECK(r.progress().find(", listing-bound ") != std::string::npos);
  CHECK(r.summary(false).find("more --checkers may help") != std::string::npos);
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

TEST_CASE("fake: a shard that cannot walk a subtree for an entry in the way fails it") {
  // Shard 1 of 2 owns the top directory, shard 0 owns a.
  FakeEndpoint src, dst;
  src.add_dir("a");
  src.add_file("a/f", "in a");
  dst.add_file("a", "in the way");
  auto shard = [&](int k) {
    SyncOptions o = fake_options();
    o.delete_extra = true;
    o.shard_index = k;
    o.shard_count = 2;
    Report r;
    REQUIRE(run_sync(src, dst, o, r).ok());
    return r.stats.failures.load();
  };
  CHECK(shard(0) == 1);
  CHECK(shard(1) == 0);  // replaces the file, but a/f is shard 0's
  CHECK_FALSE(dst.get("a/f"));
  CHECK(shard(0) == 0);
  expect_mirrored(src, dst);
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
  o.min_transfers = o.max_transfers = 1;
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
  auto failures = j.failures().value();
  REQUIRE(failures.size() == 1);
  CHECK(failures[0].path == "d1");
  CHECK(failures[0].type == EntryType::Directory);
  CHECK(j.is_done_dir("").value());
  CHECK_FALSE(j.is_done_dir("d1").value());

  SUBCASE("retrying the failures processes only them") {
    REQUIRE(j.begin_run(false).ok());
    Report r2;
    Cancellation cancel;
    Engine engine(src, dst, fake_options(), r2, &j, cancel);
    REQUIRE(engine.run(j.failures().value()).ok());
    REQUIRE(j.end_run(true).ok());
    CHECK(r2.stats.files_copied == 1);  // d1/f1; f0 was not looked at
    CHECK(r2.stats.files_checked == 2);
    CHECK(r2.stats.failures == 1);      // d1/d2/f2 still fails
    failures = j.failures().value();
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
    REQUIRE(engine3.run(j.failures().value()).ok());
    CHECK(r3.stats.files_copied == 1);
    CHECK(r3.stats.files_checked == 1);
    CHECK(j.failures().value().empty());
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
    REQUIRE(engine.run(j.failures().value()).ok());
    CHECK(j.failures().value().size() == 1);
    CHECK(j.failures().value()[0].path == "d1");
  }

  SUBCASE("a journal refuses another source/target pair") {
    auto other = Journal::open(tmp.sub("journal.sqlite"), "fake", "elsewhere");
    REQUIRE_FALSE(other.ok());
  }
}

TEST_CASE("fake: directories are recorded as finalized once the target has synced their copies") {
  TempDir tmp;
  auto journal = Journal::open(tmp.sub("j.sqlite"), "fake", "fake");
  REQUIRE(journal.ok());
  Journal& j = *journal.value();
  FakeEndpoint src, dst;
  populate(src);
  // Copies that take a while, and a sync every 20 ms meanwhile.
  src.hook = [](std::string_view op, const RelPath&) {
    if (op == "read") std::this_thread::sleep_for(std::chrono::milliseconds(30));
  };
  SyncOptions o = fake_options();
  o.sync_interval = std::chrono::milliseconds(20);
  REQUIRE(j.begin_run(false).ok());
  Report r;
  REQUIRE(run_sync(src, dst, o, r, &j).ok());
  REQUIRE(j.end_run(false).ok());
  CHECK(dst.count("flush") >= 3);
  for (const char* dir : {"", "d1", "d1/d2"}) CHECK(j.is_done_dir(dir).value());

  // A sync that fails records none of them, so that resuming walks them.
  src.hook = nullptr;
  dst.fail("flush", "", Error{ErrorKind::IO, "sync failed"}, 1000);
  REQUIRE(j.begin_run(false).ok());
  Report r2;
  REQUIRE(run_sync(src, dst, o, r2, &j).ok());
  REQUIRE(j.end_run(false).ok());
  CHECK(r2.stats.failures >= 1);
  for (const char* dir : {"", "d1", "d1/d2"}) CHECK_FALSE(j.is_done_dir(dir).value());
}

TEST_CASE("fake: a run stops when its journal cannot be written") {
  TempDir tmp;
  auto journal = Journal::open(tmp.sub("j.sqlite"), "fake", "fake");
  REQUIRE(journal.ok());
  Journal& j = *journal.value();
  REQUIRE(j.begin_run(false).ok());
  // As if the disk were full when the first failure is recorded.
  sqlite3* db = nullptr;
  REQUIRE(sqlite3_open(tmp.sub("j.sqlite").c_str(), &db) == SQLITE_OK);
  REQUIRE(sqlite3_exec(db, "CREATE TRIGGER full BEFORE INSERT ON failures BEGIN "
                           "SELECT RAISE(FAIL, 'database or disk is full'); END;",
                       nullptr, nullptr, nullptr) == SQLITE_OK);
  sqlite3_close(db);
  FakeEndpoint src, dst;
  populate(src);
  src.fail("open_read", "f0", Error{ErrorKind::Permission, "denied"});
  Report r;
  Status s = run_sync(src, dst, fake_options(), r, &j);
  REQUIRE_FALSE(s.ok());
  CHECK(s.error().kind != ErrorKind::Cancelled);
  CHECK(s.error().message.find("full") != std::string::npos);
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
  REQUIRE(j.failures().value().size() == 1);
  CHECK(j.failures().value()[0].path == "extra");

  // A metadata-only fix of a copied file: the owner differs after the copy.
  dst.modify("d1/f1", [](FakeEndpoint::Node& n) { n.entry.uid = 7; });
  dst.fail("set_metadata", "d1/f1", Error{ErrorKind::Permission, "locked"}, 1);
  REQUIRE(j.begin_run(false).ok());
  Report r2;
  REQUIRE(run_sync(src, dst, o, r2, &j).ok());
  REQUIRE(j.end_run(true).ok());
  CHECK(r2.stats.deleted == 1);  // "extra" now
  CHECK(r2.stats.failures == 1);
  REQUIRE(j.failures().value().size() == 1);
  CHECK(j.failures().value()[0].path == "d1/f1");

  REQUIRE(j.begin_run(false).ok());
  Report r3;
  REQUIRE(run_sync(src, dst, o, r3, &j).ok());
  REQUIRE(j.end_run(true).ok());
  CHECK(r3.stats.metadata_fixed == 1);
  CHECK(j.failures().value().empty());

  // A failure whose source vanishes is dropped when retried.
  src.fail("open_read", "f0", Error{ErrorKind::Permission, "denied"}, 1);
  src.modify("f0", [](FakeEndpoint::Node& n) { n.entry.mtime = {3333, 0}; });
  REQUIRE(j.begin_run(false).ok());
  Report r4;
  REQUIRE(run_sync(src, dst, o, r4, &j).ok());
  REQUIRE(j.end_run(true).ok());
  REQUIRE(j.failures().value().size() == 1);
  // Gone from the source and from the target: the retry drops the row.
  FakeEndpoint src3;
  populate(src3);
  REQUIRE(src3.remove("f0", EntryType::File).ok());
  REQUIRE(dst.remove("f0", EntryType::File).ok());
  REQUIRE(j.begin_run(false).ok());
  Report r5;
  Engine engine(src3, dst, o, r5, &j, cancel);
  REQUIRE(engine.run(j.failures().value()).ok());
  REQUIRE(j.end_run(true).ok());
  CHECK(j.failures().value().empty());
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

TEST_CASE("fake: every deletion that the cap withholds is journaled, and told once") {
  TempDir tmp;
  auto journal = Journal::open(tmp.sub("j.sqlite"), "fake", "fake");
  REQUIRE(journal.ok());
  Journal& j = *journal.value();
  FakeEndpoint src, dst;
  populate(src);
  for (int i = 0; i < 5; ++i) dst.add_file("extra" + std::to_string(i), "x");
  SyncOptions o = fake_options();
  o.delete_extra = true;
  o.max_delete = 2;
  o.max_consecutive_failures = 2;  // withheld deletions do not count toward it
  REQUIRE(j.begin_run(false).ok());
  Report r;
  StderrCapture err;
  REQUIRE(run_sync(src, dst, o, r, &j).ok());
  std::string text = err.text();
  REQUIRE(j.end_run(true).ok());
  CHECK(r.stats.deleted == 2);
  CHECK(r.stats.failures == 3);
  CHECK(j.failures().value().size() == 3);
  size_t told = 0;
  for (size_t at = 0; (at = text.find("deletion cap", at)) != std::string::npos; ++at) ++told;
  CHECK(told == 1);

  o.max_delete = 3;
  REQUIRE(j.begin_run(false).ok());
  Report r2;
  Cancellation cancel;
  Engine engine(src, dst, o, r2, &j, cancel);
  REQUIRE(engine.run(j.failures().value()).ok());
  REQUIRE(j.end_run(true).ok());
  CHECK(r2.stats.deleted == 3);
  CHECK(j.failures().value().empty());
  expect_mirrored(src, dst);
  CHECK(dst.paths().size() == src.paths().size());
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

TEST_CASE("fake: targets without mtimes need --size-only, targets without symlinks skip them") {
  FakeEndpoint src, dst;
  dst.caps.can_set_mtime = false;
  dst.caps.has_symlinks = false;
  populate(src);
  Report refused;
  Status s = run_sync(src, dst, fake_options(), refused);
  REQUIRE_FALSE(s.ok());
  CHECK(s.error().message.find("--size-only") != std::string::npos);
  CHECK(dst.paths().empty());

  SyncOptions o = fake_options();
  o.size_only = true;
  Report r;
  REQUIRE(run_sync(src, dst, o, r).ok());
  CHECK(r.stats.files_copied == 3);
  CHECK(r.stats.symlinks_skipped == 1);
  CHECK(r.stats.symlinks_created == 0);
  CHECK(r.stats.failures == 0);
  CHECK_FALSE(dst.get("d1/l"));

  src.modify("f0", [](FakeEndpoint::Node& n) { n.entry.mtime = {9999, 0}; });  // same size
  Report r2;
  REQUIRE(run_sync(src, dst, o, r2).ok());
  CHECK(r2.stats.files_copied == 0);
  CHECK(r2.stats.files_unchanged == 3);
  CHECK(r2.stats.metadata_fixed == 0);
}

TEST_CASE("fake: --size-only on a target with mtimes copies by size and sets mtimes") {
  FakeEndpoint src, dst;
  populate(src);
  Report mirror;
  REQUIRE(run_sync(src, dst, fake_options(), mirror).ok());
  src.modify("f0", [](FakeEndpoint::Node& n) {
    n.content = "root fil3";  // the same size
    n.entry.mtime = {9999, 0};
  });
  src.modify("d1/d2/f2", [](FakeEndpoint::Node& n) {
    n.content = "deeper";
    n.entry.size = 6;
  });
  SyncOptions o = fake_options();
  o.size_only = true;
  Report r;
  REQUIRE(run_sync(src, dst, o, r).ok());
  CHECK(r.stats.files_copied == 1);
  CHECK(r.stats.files_unchanged == 2);
  CHECK(r.stats.metadata_fixed == 2);  // f0, and d1/d2 after its copy
  CHECK(dst.get("f0")->content == "root file");
  CHECK(dst.get("f0")->entry.mtime == Timespec{9999, 0});
  CHECK(dst.get("d1/d2/f2")->content == "deeper");
}

TEST_CASE("fake: --delete on a target that follows symlinks it cannot show needs to be told") {
  FakeEndpoint src, dst;
  dst.caps.follows_symlinks = true;  // like a plain XRootD server
  populate(src);
  dst.add_file("extra", "x");
  SyncOptions o = fake_options();
  o.delete_extra = true;
  Report r;
  Status s = run_sync(src, dst, o, r);
  REQUIRE_FALSE(s.ok());
  CHECK(s.error().message.find("--target-without-symlinks") != std::string::npos);
  CHECK(dst.get("extra"));
  CHECK_FALSE(dst.get("f0"));

  o.target_without_symlinks = true;
  Report r2;
  REQUIRE(run_sync(src, dst, o, r2).ok());
  CHECK(r2.stats.deleted == 1);
  expect_mirrored(src, dst);

  // Without --delete, such a target needs no flag, and nothing is deleted.
  dst.add_file("extra", "x");
  Report r3;
  REQUIRE(run_sync(src, dst, fake_options(), r3).ok());
  CHECK(r3.stats.deleted == 0);
  CHECK(dst.get("extra"));
}

TEST_CASE("fake: --one-file-system skips directories on other file systems") {
  FakeEndpoint src, dst;
  populate(src);
  src.add_dir("mnt");
  src.add_file("mnt/data", "elsewhere");
  src.modify("mnt", [](FakeEndpoint::Node& n) { n.entry.id = "other-fs"; });
  dst.add_dir("mnt");
  dst.add_file("mnt/old", "kept");
  SyncOptions o = fake_options();
  o.delete_extra = true;
  o.one_file_system = true;
  Report r;
  REQUIRE(run_sync(src, dst, o, r).ok());
  CHECK(r.stats.other_fs_skipped == 1);
  CHECK(r.stats.failures == 0);
  CHECK(r.stats.deleted == 0);
  CHECK(dst.get("mnt/old"));
  CHECK_FALSE(dst.get("mnt/data"));
  CHECK(dst.get("d1/d2/f2"));
  CHECK(r.summary(false).find("other file systems skipped (--one-file-system): 1") !=
        std::string::npos);

  // Nor is it created where the target lacks it.
  dst.erase("mnt");
  Report r2;
  REQUIRE(run_sync(src, dst, o, r2).ok());
  CHECK_FALSE(dst.get("mnt"));

  o.one_file_system = false;
  Report r3;
  REQUIRE(run_sync(src, dst, o, r3).ok());
  CHECK(dst.get("mnt/data"));
}

TEST_CASE("fake: files replaced where the target forces non-atomic uploads are told and counted") {
  FakeEndpoint src, dst;
  populate(src);
  src.add_file("d1/f3", "three");
  Report mirror;
  REQUIRE(run_sync(src, dst, fake_options(), mirror).ok());
  dst.replacing_at_open = {"d1"};
  // New files there need no warning.
  src.add_file("d1/new", "new");
  StderrCapture new_only;
  Report r;
  REQUIRE(run_sync(src, dst, fake_options(), r).ok());
  CHECK(r.stats.files_copied == 1);
  CHECK(r.stats.non_atomic_replacements == 0);
  CHECK(new_only.text().find("without atomic uploads") == std::string::npos);

  // Replacing two is told once, and both are replaced all the same.
  for (const char* path : {"d1/f1", "d1/f3"})
    src.modify(path, [](FakeEndpoint::Node& n) { n.entry.mtime = {7777, 0}; });
  src.modify("f0", [](FakeEndpoint::Node& n) { n.entry.mtime = {7777, 0}; });
  StderrCapture err;
  Report r2;
  REQUIRE(run_sync(src, dst, fake_options(), r2).ok());
  std::string text = err.text();
  CHECK(r2.stats.files_copied == 3);
  CHECK(r2.stats.non_atomic_replacements == 2);
  CHECK(r2.stats.failures == 0);
  size_t told = 0;
  for (size_t at = 0; (at = text.find("without atomic uploads", at)) != std::string::npos; ++at) ++told;
  CHECK(told == 1);
  CHECK(text.find("replacing files in d1 without atomic uploads") != std::string::npos);
  CHECK(r2.summary(false).find("files replaced without atomic uploads (forced by the target): 2") !=
        std::string::npos);
  expect_mirrored(src, dst);
}

TEST_CASE("fake: --checksum compares files of the same size by checksum") {
  FakeEndpoint src, dst;
  populate(src);
  Report mirror;
  REQUIRE(run_sync(src, dst, fake_options(), mirror).ok());
  // Other content, the same size and mtime; the same content, another mtime.
  dst.modify("f0", [](FakeEndpoint::Node& n) { n.content = "root fil3"; });
  dst.modify("d1/d2/f2", [](FakeEndpoint::Node& n) { n.entry.mtime = {1, 0}; });
  SyncOptions o = fake_options();
  o.checksum = true;

  SUBCASE("by the checksums that both listings carry") {
    for (FakeEndpoint* ep : {&src, &dst}) {
      ep->caps.checksum = ChecksumType::Adler32;
      ep->lists_checksums = true;
    }
    Report r;
    REQUIRE(run_sync(src, dst, o, r).ok());
    CHECK(r.stats.checksums_compared == 3);
    CHECK(r.stats.checksums_differ == 1);
    CHECK(r.stats.files_copied == 1);
    CHECK(dst.count("open_read") == 0);
  }
  SUBCASE("by reading files that have none") {
    Report r;
    REQUIRE(run_sync(src, dst, o, r).ok());
    CHECK(r.stats.checksums_compared == 3);
    CHECK(r.stats.checksums_differ == 1);
    CHECK(r.stats.files_copied == 1);
    CHECK(dst.count("open_read") == 3);
  }
  SUBCASE("by reading the side that has none") {
    dst.caps.checksum = ChecksumType::Adler32;
    dst.lists_checksums = true;
    Report r;
    REQUIRE(run_sync(src, dst, o, r).ok());
    CHECK(r.stats.checksums_differ == 1);
    CHECK(r.stats.files_copied == 1);
    CHECK(dst.count("open_read") == 0);
  }
  // The file that matched got its mtime.
  CHECK(dst.get("d1/d2/f2")->entry.mtime == src.get("d1/d2/f2")->entry.mtime);
  expect_mirrored(src, dst);
}

TEST_CASE("fake: a differing file that mtime 0 cannot mark cannot pass for a copy later") {
  FakeEndpoint src, dst;
  populate(src);
  SyncOptions plain = fake_options();
  SUBCASE("a source file with mtime 0") {
    src.modify("f0", [](FakeEndpoint::Node& n) { n.entry.mtime = {0, 0}; });
  }
  SUBCASE("a target without mtimes") {
    dst.caps.can_set_mtime = false;
    plain.size_only = true;
  }
  Report mirror;
  REQUIRE(run_sync(src, dst, plain, mirror).ok());
  // Other content with the same size and mtime, and a new copy that fails.
  dst.modify("f0", [](FakeEndpoint::Node& n) { n.content = "root fil3"; });
  dst.fail("commit", "f0", Error{ErrorKind::NoSpace, "quota exceeded"});
  SyncOptions o = fake_options();
  o.checksum = true;
  Report r;
  REQUIRE(run_sync(src, dst, o, r).ok());
  CHECK(r.stats.checksums_differ == 1);
  CHECK(r.stats.failures == 1);
  // A run that compares by size (and mtime) copies it all the same.
  Report again;
  REQUIRE(run_sync(src, dst, plain, again).ok());
  CHECK(again.stats.files_copied == 1);
  CHECK(dst.get("f0")->content == "root file");
}

TEST_CASE("fake: no file is removed to mark it where that may delete through a symlink") {
  FakeEndpoint src, dst;
  populate(src);
  dst.caps.can_set_mtime = false;
  dst.caps.follows_symlinks = true;
  SyncOptions o = fake_options();
  o.size_only = true;
  Report mirror;
  REQUIRE(run_sync(src, dst, o, mirror).ok());
  dst.modify("f0", [](FakeEndpoint::Node& n) { n.content = "root fil3"; });
  dst.fail("commit", "f0", Error{ErrorKind::NoSpace, "quota exceeded"});
  o.size_only = false;
  o.checksum = true;
  Report r;
  REQUIRE(run_sync(src, dst, o, r).ok());
  CHECK(r.stats.failures == 1);
  CHECK(dst.count("remove") == 0);
  CHECK(dst.get("f0"));
}

TEST_CASE("fake: --dry-run --checksum verifies, and a later run repairs what differs") {
  TempDir tmp;
  auto journal = Journal::open(tmp.sub("j.sqlite"), "fake", "fake");
  REQUIRE(journal.ok());
  Journal& j = *journal.value();
  FakeEndpoint src, dst;
  populate(src);
  Report mirror;
  REQUIRE(run_sync(src, dst, fake_options(), mirror).ok());
  dst.modify("f0", [](FakeEndpoint::Node& n) { n.content = "root fil3"; });
  dst.erase("d1/d2/f2");
  dst.modify("d1/f1", [](FakeEndpoint::Node& n) {
    n.content.pop_back();
    n.entry.size = n.content.size();
  });
  SyncOptions o = fake_options();
  o.checksum = true;
  o.dry_run = true;
  Report r;
  REQUIRE(run_sync(src, dst, o, r, &j).ok());
  CHECK(r.stats.checksums_compared == 1);
  CHECK(r.stats.checksums_differ == 1);
  std::map<RelPath, std::string> reported;
  for (const Failure& f : r.failures()) {
    CHECK(f.error.kind == ErrorKind::Checksum);
    reported[f.path] = f.error.message;
  }
  REQUIRE(reported.size() == 3);
  CHECK(reported["f0"] == "differs from the source: adler32 " + adler_of("root file") +
                              " on the source, " + adler_of("root fil3") +
                              " on the target (9 bytes)");
  CHECK(reported["d1/d2/f2"] == "differs from the source: missing on the target");
  CHECK(reported["d1/f1"] ==
        "differs from the source: 200000 bytes on the source, 199999 on the target");
  CHECK(r.summary(true).find("compared by checksum: 1, differing: 1") != std::string::npos);
  CHECK(dst.get("f0")->content == "root fil3");
  CHECK(j.failures().value().size() == 3);

  // A run with the journal repairs them, also without --checksum.
  REQUIRE(j.begin_run(false).ok());
  Report repaired;
  REQUIRE(run_sync(src, dst, fake_options(), repaired, &j).ok());
  REQUIRE(j.end_run(true).ok());
  CHECK(repaired.stats.files_copied == 3);
  CHECK(j.failures().value().empty());
  expect_mirrored(src, dst);
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

TEST_CASE("fake: modes are left alone on a target that sets none") {
  FakeEndpoint src, dst;
  // Without the privilege to set owners, read-only target directories would
  // be made writable by setting their mode.
  dst.caps.can_set_mode = false;
  dst.caps.can_set_owner = false;
  populate(src);
  src.add_dir("ro", 0555);
  src.add_file("ro/f", "x", 0444);
  dst.add_dir("extra", 0555);
  dst.add_file("extra/f", "x");
  SyncOptions o = test_options();
  o.preserve_owner = false;
  o.delete_extra = true;
  Report r;
  StderrCapture err;
  REQUIRE(run_sync(src, dst, o, r).ok());
  CHECK(err.text().find("sets no modes") != std::string::npos);
  CHECK(r.stats.failures == 0);
  CHECK(r.stats.files_copied == 4);
  CHECK(dst.get("ro/f")->content == "x");
  CHECK(dst.get("ro")->entry.mtime == src.get("ro")->entry.mtime);
  CHECK_FALSE(dst.get("extra"));
  Report r2;
  REQUIRE(run_sync(src, dst, o, r2).ok());
  CHECK(r2.stats.failures == 0);
  CHECK(r2.stats.files_copied == 0);
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

TEST_CASE("fake: temporaries are stale once neither their mtime nor their ctime is recent") {
  FakeEndpoint src, dst;
  populate(src);
  int64_t now = std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::system_clock::now().time_since_epoch())
                    .count();
  // A leftover of an earlier run, an upload in progress that EOS gave the
  // source's mtime when it created it, and a temporary being written.
  dst.add_file(".tmp-left", "partial", 0600, {now - 7200, 0}).ctime = {now - 7200, 0};
  dst.add_file(".tmp-upload", "in flight", 0600, {1500000000, 0}).ctime = {now - 60, 0};
  dst.add_file(".tmp-written", "in progress", 0600, {now - 10, 0});
  Report r;
  REQUIRE(run_sync(src, dst, fake_options(), r).ok());
  CHECK(r.stats.extras == 0);
  CHECK(r.stats.stale_temps == 1);

  SyncOptions o = fake_options();
  o.delete_extra = true;
  Report r2;
  REQUIRE(run_sync(src, dst, o, r2).ok());
  CHECK(r2.stats.deleted == 1);
  CHECK_FALSE(dst.get(".tmp-left"));
  CHECK(dst.get(".tmp-upload"));
  CHECK(dst.get(".tmp-written"));
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

TEST_CASE("fake: directories and symlinks named like temporaries are entries of the tree") {
  FakeEndpoint src, dst;
  populate(src);
  src.add_dir(".tmp-dir");
  src.add_file(".tmp-dir/f", "user data");
  src.add_symlink(".tmp-link", "f0");
  Report r;
  REQUIRE(run_sync(src, dst, fake_options(), r).ok());
  CHECK(r.stats.failures == 0);
  expect_mirrored(src, dst);

  // On the target, they count for a directory that the source emptied.
  src.add_dir("emptied");
  dst.add_dir("emptied");
  dst.add_dir("emptied/.tmp-old", 0755, {1000, 0});
  SyncOptions o = fake_options();
  o.delete_extra = true;
  o.keep_emptied = true;
  Report r2;
  REQUIRE(run_sync(src, dst, o, r2).ok());
  CHECK(r2.stats.emptied_dirs == 1);
  CHECK(r2.stats.deleted == 0);
  CHECK(dst.get("emptied/.tmp-old"));
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
  o.min_transfers = 2;
  o.max_transfers = 6;
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
  // Files not mirrored count as a failure of their directory, as with rsync.
  Report r;
  REQUIRE(run_sync(src, dst, fake_options(), r).ok());
  CHECK(r.stats.invalid_names == bad.size());
  REQUIRE(r.failures().size() == 1);
  CHECK(r.failures()[0].path == "");
  CHECK(r.failures()[0].type == EntryType::Directory);
  CHECK(r.stats.files_copied == 3);
  CHECK(dst.get("")->entry.type == EntryType::Directory);
  CHECK(dst.paths() == std::vector<RelPath>{"d1", "d1/d2", "d1/d2/f2", "d1/f1", "d1/l", "f0"});
  CHECK(r.summary(false).find("entries with invalid names skipped: 7") != std::string::npos);

  // On the target, such entries are neither deleted nor descended into, and
  // nothing else is deleted where a listing had them.
  dst.add_file("junk", "x");
  dst.modify("junk", [](FakeEndpoint::Node& n) { n.entry.name = ".."; });
  dst.add_file("extra", "x");
  dst.add_dir("d1/extra");
  dst.add_file("d1/extra/inner", "x");
  dst.modify("d1/extra/inner", [](FakeEndpoint::Node& n) { n.entry.name = "../../d1"; });
  SyncOptions o = fake_options();
  o.delete_extra = true;
  Report r2;
  REQUIRE(run_sync(src, dst, o, r2).ok());
  CHECK(r2.stats.invalid_names == bad.size() + 2);
  CHECK(r2.stats.incomplete_dirs == 1);
  CHECK(r2.stats.deleted == 0);
  REQUIRE(r2.failures().size() == 2);
  for (const Failure& f : r2.failures()) {
    INFO("failure of ", f.path);
    CHECK(f.error.kind == (f.path.empty() ? ErrorKind::Other : ErrorKind::NotEmpty));
    CHECK((f.path.empty() || f.path == "d1/extra"));
  }
  CHECK(dst.get("junk"));
  CHECK(dst.get("extra"));
  CHECK(dst.get("d1/extra/inner"));
  CHECK(dst.get("d1"));
}

TEST_CASE("fake: nothing is deleted where a listing skipped entries, which is a failure") {
  TempDir tmp;
  auto journal = Journal::open(tmp.sub("j.sqlite"), "fake", "fake");
  REQUIRE(journal.ok());
  Journal& j = *journal.value();
  FakeEndpoint src, dst;
  populate(src);
  Report mirror;
  REQUIRE(run_sync(src, dst, fake_options(), mirror).ok());
  // The source's listing of d1 leaves out f1 as unsafe, like a line of an EOS
  // listing that cannot be parsed, and the target's listing of d1/d2 leaves
  // out an entry of its own. Their names may be on the other side.
  src.unlistable.insert("d1/f1");
  dst.add_file("d1/extra", "x");
  dst.add_file("d1/d2/extra", "x");
  dst.add_file("d1/d2/hidden", "x");
  dst.unlistable.insert("d1/d2/hidden");
  dst.add_file("extra", "x");
  SyncOptions o = fake_options();
  o.delete_extra = true;
  REQUIRE(j.begin_run(false).ok());
  Report r;
  REQUIRE(run_sync(src, dst, o, r, &j).ok());
  REQUIRE(j.end_run(true).ok());
  CHECK(r.stats.deleted == 1);
  CHECK(r.stats.incomplete_dirs == 2);
  CHECK(dst.get("d1/f1"));
  CHECK(dst.get("d1/extra"));
  CHECK(dst.get("d1/d2/extra"));
  CHECK_FALSE(dst.get("extra"));
  CHECK(r.summary(false).find("could not be listed, whose target entries were kept: 2") !=
        std::string::npos);
  // Failures of their directories, as with rsync: what the source's listing
  // left out is not copied, and the deletions are withheld.
  CHECK(r.stats.failures == 2);
  auto rows = j.failures().value();
  REQUIRE(rows.size() == 2);
  CHECK(rows[0].path == "d1");
  CHECK(rows[0].error.kind == ErrorKind::Other);
  CHECK(rows[1].path == "d1/d2");
  CHECK(rows[1].error.kind == ErrorKind::Withheld);

  // Retrying them once the listings are whole deletes there.
  src.unlistable.clear();
  dst.unlistable.clear();
  REQUIRE(j.begin_run(false).ok());
  Report retried;
  Cancellation cancel;
  Engine engine(src, dst, o, retried, &j, cancel);
  REQUIRE(engine.run(j.failures().value()).ok());
  REQUIRE(j.end_run(true).ok());
  CHECK(retried.stats.deleted == 3);
  CHECK(retried.stats.failures == 0);
  CHECK(j.failures().value().empty());
  expect_mirrored(src, dst);

  // Where nothing would be deleted, nothing is held back either.
  dst.unlistable = {"d1/f1"};
  int64_t now = std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::system_clock::now().time_since_epoch())
                    .count();
  dst.add_file("d1/.tmp-upload", "in progress", 0600, {now, 0});
  Report r2;
  REQUIRE(run_sync(src, dst, o, r2).ok());
  CHECK(r2.stats.incomplete_dirs == 0);
  CHECK(r2.stats.failures == 0);
  CHECK(dst.get("d1/.tmp-upload"));
}

TEST_CASE("fake: no type conflict is resolved and no tree deleted where a listing skipped entries") {
  FakeEndpoint src, dst;
  src.add_dir("d");
  src.add_file("d/x", "file");
  src.add_file("d/y", "y");
  src.unlistable.insert("d/y");
  dst.add_dir("d");
  dst.add_dir("d/x");
  dst.add_file("d/x/inner", "kept");
  // An extra tree with a directory whose listing skips an entry.
  dst.add_dir("gone");
  dst.add_dir("gone/sub");
  dst.add_file("gone/sub/a", "a");
  dst.add_file("gone/sub/hidden", "h");
  dst.unlistable.insert("gone/sub/hidden");
  SyncOptions o = fake_options();
  o.delete_extra = true;
  Report r;
  REQUIRE(run_sync(src, dst, o, r).ok());
  CHECK(r.stats.incomplete_dirs == 1);
  CHECK(r.stats.deleted == 0);
  CHECK(dst.get("d/x/inner"));
  CHECK(dst.get("gone/sub/a"));
  // And d, whose listing left out y.
  REQUIRE(r.failures().size() == 3);
  for (const Failure& f : r.failures()) {
    INFO("failure of ", f.path);
    CHECK((f.path == "d" || f.path == "d/x" || f.path == "gone"));
  }
}

TEST_CASE("fake: the source confirms what it lacks before anything is deleted") {
  FakeEndpoint src, dst;
  populate(src);
  src.add_file("c", "file");
  Report mirror;
  REQUIRE(run_sync(src, dst, fake_options(), mirror).ok());
  // The source's listing of d1 leaves out f1 without saying so.
  src.unlisted.insert("d1/f1");
  dst.add_file("d1/extra", "x");
  SyncOptions o = fake_options();
  o.delete_extra = true;
  for (bool dry_run : {true, false}) {
    o.dry_run = dry_run;
    Report r;
    REQUIRE(run_sync(src, dst, o, r).ok());
    CHECK(r.stats.incomplete_dirs == 1);
    CHECK(r.stats.deleted == 0);
    REQUIRE(r.failures().size() == 1);
    CHECK(r.failures()[0].path == "d1");
    CHECK(r.failures()[0].error.kind == ErrorKind::Withheld);
  }
  CHECK(dst.get("d1/f1"));
  CHECK(dst.get("d1/extra"));

  // Nor where the source cannot tell.
  src.unlisted.clear();
  src.fail("stat", "d1/extra", Error{ErrorKind::IO, "no answer"}, o.retry.attempts);
  Report r2;
  REQUIRE(run_sync(src, dst, o, r2).ok());
  CHECK(r2.stats.incomplete_dirs == 1);
  CHECK(r2.stats.failures == 1);
  CHECK(dst.get("d1/extra"));
  Report r3;
  REQUIRE(run_sync(src, dst, o, r3).ok());
  CHECK(r3.stats.incomplete_dirs == 0);
  CHECK(r3.stats.deleted == 1);
  CHECK_FALSE(dst.get("d1/extra"));

  // An entry in the way of another type is replaced only while the source's
  // entry has the type listed.
  dst.erase("c");
  dst.add_dir("c");
  dst.add_file("c/inner", "kept");
  dst.add_file("extra", "x");
  src.hook = [&](std::string_view op, const RelPath& path) {
    if (op == "stat" && path == "c")
      src.modify("c", [](FakeEndpoint::Node& n) { n.entry.type = EntryType::Directory; });
  };
  Report r4;
  REQUIRE(run_sync(src, dst, o, r4).ok());
  CHECK(r4.stats.incomplete_dirs == 1);
  CHECK(r4.stats.deleted == 0);
  REQUIRE(r4.failures().size() == 2);
  for (const Failure& f : r4.failures())
    CHECK(f.error.kind == (f.path == "c" ? ErrorKind::Changed : ErrorKind::Withheld));
  CHECK(dst.get("c/inner"));
  CHECK(dst.get("extra"));
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

TEST_CASE("fake: a source whose readers take the listing is copied without stats") {
  // Like EOS, the source lists the checksums it stores.
  auto store_checksum = [](FakeEndpoint::Node& n) {
    if (n.entry.type != EntryType::File) return;
    Hasher h(ChecksumType::Adler32);
    h.update({reinterpret_cast<const std::byte*>(n.content.data()), n.content.size()});
    n.entry.checksum = h.finish();
  };
  FakeEndpoint src;
  src.readers_take_listing = true;
  populate(src);
  for (const RelPath& p : src.paths()) src.modify(p, store_checksum);
  auto replace = [&](const RelPath& path, std::string content, Timespec mtime) {
    src.modify(path, [&](FakeEndpoint::Node& n) {
      n.entry.size = content.size();
      n.content = std::move(content);
      n.entry.mtime = mtime;
      store_checksum(n);
    });
  };
  FakeEndpoint dst;
  Report r;
  REQUIRE(run_sync(src, dst, fake_options(), r).ok());
  CHECK(r.stats.files_copied == 3);
  CHECK(r.stats.files_unverified == 0);
  CHECK(src.count("stat") == 1);  // the top directory
  expect_mirrored(src, dst);

  SUBCASE("a file of another size than listed is copied as it is now") {
    src.hook = [&](std::string_view op, const RelPath& path) {
      if (op == "open_read" && path == "f0") replace("f0", "rewritten now", {3500, 0});
    };
    FakeEndpoint dst2;
    Report r2;
    REQUIRE(run_sync(src, dst2, fake_options(), r2).ok());
    CHECK(r2.stats.failures == 0);
    CHECK(r2.stats.retries == 0);
    CHECK(dst2.get("f0")->content == "rewritten now");
    CHECK(dst2.get("f0")->entry.mtime == Timespec{3500, 0});
    CHECK(src.count("stat") == 1 + 1 + 2);  // and before and after reading f0
  }

  SUBCASE("a file that grows while it is read is copied again") {
    int reads = 0;
    src.hook = [&](std::string_view op, const RelPath& path) {
      if (op == "read" && path == "d1/f1" && reads++ == 0)
        replace("d1/f1", std::string(200001, 'y'), {3600, 0});
    };
    FakeEndpoint dst2;
    Report r2;
    REQUIRE(run_sync(src, dst2, fake_options(), r2).ok());
    CHECK(r2.stats.failures == 0);
    CHECK(r2.stats.retries == 1);
    CHECK(dst2.get("d1/f1")->content == std::string(200001, 'y'));
    CHECK(dst2.get("d1/f1")->entry.mtime == Timespec{3600, 0});
  }

  SUBCASE("a file rewritten with its size since the listing is a change, not corruption") {
    src.hook = [&](std::string_view op, const RelPath& path) {
      if (op == "open_read" && path == "f0") replace("f0", "ROOT FILE", {3700, 0});
    };
    FakeEndpoint dst2;
    Report r2;
    REQUIRE(run_sync(src, dst2, fake_options(), r2).ok());
    REQUIRE(r2.stats.failures == 1);
    CHECK(r2.failures()[0].path == "f0");
    CHECK(r2.failures()[0].error.kind == ErrorKind::Changed);
    CHECK(r2.failures()[0].error.message.find("since it was listed") != std::string::npos);
    CHECK_FALSE(dst2.get("f0"));

    // The next run lists it anew.
    src.hook = nullptr;
    Report r3;
    REQUIRE(run_sync(src, dst2, fake_options(), r3).ok());
    CHECK(r3.stats.failures == 0);
    CHECK(dst2.get("f0")->content == "ROOT FILE");
  }

  SUBCASE("data that does not match the listed checksum of an unchanged file is corrupt") {
    src.modify("f0", [](FakeEndpoint::Node& n) { n.content[0] = 'R'; });
    FakeEndpoint dst2;
    Report r2;
    REQUIRE(run_sync(src, dst2, fake_options(), r2).ok());
    REQUIRE(r2.stats.failures == 1);
    CHECK(r2.failures()[0].error.kind == ErrorKind::Checksum);
    CHECK_FALSE(dst2.get("f0"));
  }
}

TEST_CASE("fake: entries that vanish from the source before they are handled are no failures") {
  TempDir tmp;
  auto journal = Journal::open(tmp.sub("j.sqlite"), "fake", "fake");
  REQUIRE(journal.ok());
  Journal& j = *journal.value();
  REQUIRE(j.begin_run(false).ok());
  REQUIRE(j.record_failure({"f0", EntryType::File, Error{ErrorKind::IO, "earlier"}}).ok());
  REQUIRE(j.record_failure({"d1/d2", EntryType::Directory, Error{ErrorKind::IO, "earlier"}}).ok());
  REQUIRE(j.end_run(true).ok());

  FakeEndpoint src, dst;
  populate(src);
  // Each vanishes after its directory was listed.
  src.hook = [&](std::string_view op, const RelPath& path) {
    if (op == "open_read" && path == "f0") src.erase("f0");
    if (op == "list" && path == "d1/d2") src.erase("d1/d2");
  };
  REQUIRE(j.begin_run(false).ok());
  Report r;
  REQUIRE(run_sync(src, dst, fake_options(), r, &j).ok());
  REQUIRE(j.end_run(true).ok());
  CHECK(r.stats.vanished == 2);
  CHECK(r.stats.failures == 0);
  CHECK(r.stats.retries == 0);
  CHECK(r.stats.files_copied == 1);
  CHECK(j.failures().value().empty());
  CHECK_FALSE(dst.get("f0"));
  CHECK(r.summary(false).find("vanished from the source: 2\n") != std::string::npos);

  SUBCASE("a source that goes away as a whole stops the run") {
    FakeEndpoint src2, dst2;
    for (int i = 0; i < 40; ++i) src2.add_file("f" + std::to_string(10 + i), "x");
    src2.hook = [&](std::string_view op, const RelPath& path) {
      if (op == "open_read") src2.erase(path);
    };
    SyncOptions o = fake_options();
    o.max_consecutive_failures = 5;
    Report r2;
    Status s = run_sync(src2, dst2, o, r2);
    REQUIRE_FALSE(s.ok());
    CHECK(s.error().kind == ErrorKind::Cancelled);
    CHECK(s.error().message.find("vanished from the source") != std::string::npos);
    CHECK(r2.stats.vanished >= 5);
    CHECK(r2.stats.vanished < 40);
    CHECK(r2.stats.failures == 0);
  }

  SUBCASE("a target that has lost a file's directory is no source that vanished") {
    FakeEndpoint src2, dst2;
    src2.add_file("f", "x");
    dst2.fail("open_write", "f", Error{ErrorKind::NotFound, "no directory"});
    Report r2;
    REQUIRE(run_sync(src2, dst2, fake_options(), r2).ok());
    CHECK(r2.stats.vanished == 0);
    CHECK(r2.stats.failures == 1);
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
  CHECK(d.update(1000, 100, false) == 6);
  CHECK(d.update(500, 50, false) == 4);  // as if the rise had not paid
  int intervals = 0;
  while (d.limit() == 4 && intervals < 1000) {
    d.update(500, 50, false);
    ++intervals;
  }
  CHECK(d.limit() > 4);
  CHECK(intervals > 50);
  CHECK(intervals < 300);
}

TEST_CASE("the transfer controller takes back a rise that did not pay") {
  TransferController c(4, 64);
  CHECK(c.update(100, 10, false) == 6);
  CHECK(c.update(150, 15, false) == 9);
  // More than 10 % below the throughput at 6, in bytes and files: back to 6,
  // and no rise for six intervals, however the throughput goes.
  CHECK(c.update(130, 13, false) == 6);
  for (int i = 0; i < 6; ++i) CHECK(c.update(200 + i, 20, false) == 6);
  // Then it is tried again when throughput improves.
  CHECK(c.update(250, 25, false) == 9);
  // At a plateau, the higher limit stays.
  for (int i = 0; i < 5; ++i) CHECK(c.update(240, 24, false) == 9);
  // Retries on the target take a quarter away.
  CHECK(c.update(240, 24, true) == 7);

  // Only a drop in bytes and files takes a rise back, and only right after it.
  TransferController d(4, 64);
  CHECK(d.update(100, 10, false) == 6);
  CHECK(d.update(50, 10, false) == 6);   // files held up
  CHECK(d.update(200, 20, false) == 9);
  CHECK(d.update(205, 20, false) == 9);  // the rise neither paid nor hurt
  CHECK(d.update(100, 10, false) == 9);  // and a later drop is no reason

  // At the maximum, there is no rise to take back.
  TransferController e(4, 6);
  CHECK(e.update(100, 10, false) == 6);
  CHECK(e.update(200, 20, false) == 6);
  CHECK(e.update(50, 5, false) == 6);
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
  o.max_transfers = 5;
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
  o.max_transfers = 2;
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
  CHECK(journal.value()->failures().value().empty());
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
  REQUIRE(j.failures().value().size() == 1);
  CHECK(j.failures().value()[0].path == "");

  // Retrying the row walks the whole tree.
  for (uint64_t failures : {1, 0}) {
    REQUIRE(j.begin_run(false).ok());
    Report r2;
    Cancellation cancel;
    Engine engine(src, dst, fake_options(), r2, &j, cancel);
    REQUIRE(engine.run(j.failures().value()).ok());
    REQUIRE(j.end_run(true).ok());
    CHECK(r2.stats.dirs_listed == 3);
    CHECK(r2.stats.failures == failures);
    CHECK(j.failures().value().size() == failures);
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
  REQUIRE(j.record_failure({"d1/d2/f2", EntryType::File, Error{ErrorKind::IO, "earlier"}}).ok());
  REQUIRE(j.end_run(true).ok());

  REQUIRE(j.begin_run(false).ok());
  Report r;
  Cancellation cancel;
  Engine engine(src, dst, fake_options(), r, &j, cancel);
  REQUIRE(engine.run(j.failures().value()).ok());
  REQUIRE(j.end_run(true).ok());
  REQUIRE(r.stats.failures == 1);
  CHECK(r.failures()[0].path == "d1/d2");
  CHECK(r.failures()[0].error.kind == ErrorKind::NotFound);
  auto rows = j.failures().value();
  REQUIRE(rows.size() == 2);
  CHECK(rows[0].path == "d1/d2");
  CHECK(rows[1].path == "d1/d2/f2");

  // A full run recreates everything and clears both.
  REQUIRE(j.begin_run(false).ok());
  Report r2;
  REQUIRE(run_sync(src, dst, fake_options(), r2, &j).ok());
  REQUIRE(j.end_run(true).ok());
  CHECK(r2.stats.failures == 0);
  CHECK(j.failures().value().empty());
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
  REQUIRE(j.record_failure(
              {"d1/extra", EntryType::File, Error{ErrorKind::Permission, "locked"}})
              .ok());
  REQUIRE(j.record_failure(
              {"gone", EntryType::Directory, Error{ErrorKind::Permission, "locked"}})
              .ok());
  REQUIRE(j.record_failure({"gone/inner", EntryType::File, Error{ErrorKind::IO, "earlier"}}).ok());
  REQUIRE(j.end_run(true).ok());

  for (bool delete_extra : {false, true}) {
    SyncOptions o = fake_options();
    o.delete_extra = delete_extra;
    REQUIRE(j.begin_run(false).ok());
    Report r;
    Cancellation cancel;
    Engine engine(src, dst, o, r, &j, cancel);
    REQUIRE(engine.run(j.failures().value()).ok());
    REQUIRE(j.end_run(true).ok());
    CHECK(r.stats.failures == 0);
    CHECK(r.stats.extras == 2);
    CHECK(r.stats.deleted == (delete_extra ? 3 : 0));
    CHECK(j.failures().value().size() == (delete_extra ? 0 : 3));
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
    REQUIRE(j.record_failure({path, EntryType::File, Error{ErrorKind::IO, "earlier"}}).ok());
  REQUIRE(j.record_failure(
              {"nowhere", EntryType::Directory, Error{ErrorKind::IO, "earlier"}})
              .ok());
  REQUIRE(j.end_run(true).ok());

  REQUIRE(j.begin_run(false).ok());
  Report r;
  REQUIRE(run_sync(src, dst, fake_options(), r, &j).ok());
  REQUIRE(j.end_run(true).ok());
  // Only the extra, which a run with --delete would delete, is left.
  auto rows = j.failures().value();
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

TEST_CASE("fake: directories emptied on the source are emptied, or kept with --keep-emptied") {
  FakeEndpoint src, dst;
  populate(src);
  // An empty directory, like an unmounted automount, and an emptied one
  // deeper down, whose copies are full.
  src.add_dir("mnt");
  src.add_dir("d1/d2/sub");
  src.add_file("d1/d2/sub/.tmp-partial", "x");
  Report mirror;
  REQUIRE(run_sync(src, dst, fake_options(), mirror).ok());
  for (int i = 0; i < 50; ++i) dst.add_file("mnt/f" + std::to_string(i), "data");
  dst.add_file("d1/d2/sub/g", "data");
  dst.add_file("d1/extra", "x");

  SyncOptions o = fake_options();
  o.delete_extra = true;
  o.keep_emptied = true;
  Report r;
  REQUIRE(run_sync(src, dst, o, r).ok());
  CHECK(r.stats.failures == 0);
  CHECK(r.stats.deleted == 1);  // d1/extra
  CHECK(r.stats.extras == 52);
  CHECK(r.stats.emptied_dirs == 2);
  CHECK(dst.get("mnt/f49"));
  CHECK(dst.get("d1/d2/sub/g"));
  CHECK(r.summary(false).find("empty on the source whose target entries were kept "
                              "(--keep-emptied): 2") != std::string::npos);

  // Without --delete, nothing would be deleted anyway.
  Report r2;
  REQUIRE(run_sync(src, dst, fake_options(), r2).ok());
  CHECK(r2.stats.emptied_dirs == 0);

  // --delete alone deletes there, like rsync.
  o.keep_emptied = false;
  Report r3;
  REQUIRE(run_sync(src, dst, o, r3).ok());
  CHECK(r3.stats.deleted == 51);
  CHECK(r3.stats.emptied_dirs == 0);
  CHECK(r3.stats.failures == 0);
  CHECK_FALSE(dst.get("mnt/f0"));
  src.erase("d1/d2/sub/.tmp-partial");
  expect_mirrored(src, dst);
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
  o.min_transfers = o.max_transfers = 1;
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

TEST_CASE("fake: a copy with mtime 0 is made again, whatever the comparison says") {
  FakeEndpoint src, dst;
  populate(src);
  Report mirror;
  REQUIRE(run_sync(src, dst, fake_options(), mirror).ok());
  // A copy renamed into place before a sync made it durable, or one given
  // mtime 0 after a doubtful copy: the same size, and here other content.
  dst.modify("f0", [](FakeEndpoint::Node& n) {
    n.content = "root fil3";
    n.entry.mtime = {0, 0};
  });
  SyncOptions o = fake_options();
  SUBCASE("compared by mtime") {}
  SUBCASE("compared by size") { o.size_only = true; }
  SUBCASE("compared by checksum") {
    o.checksum = true;
    src.caps.checksum = dst.caps.checksum = ChecksumType::Adler32;
    src.lists_checksums = dst.lists_checksums = true;
    dst.modify("f0", [](FakeEndpoint::Node& n) { n.content = "root file"; });  // even equal
  }
  Report r;
  REQUIRE(run_sync(src, dst, o, r).ok());
  CHECK(r.stats.files_copied == 1);
  CHECK(r.stats.files_unchanged == 2);
  CHECK(dst.get("f0")->content == "root file");
  CHECK(dst.get("f0")->entry.mtime == Timespec{3000, 1});

  // A verifying run reports it.
  dst.modify("f0", [](FakeEndpoint::Node& n) { n.entry.mtime = {0, 0}; });
  SyncOptions verify = fake_options();
  verify.dry_run = verify.checksum = true;
  Report v;
  REQUIRE(run_sync(src, dst, verify, v).ok());
  CHECK(v.stats.failures == 1);
  CHECK(v.stats.files_copied == 1);

  // A source file with mtime 0 itself cannot be told from such a copy.
  src.modify("f0", [](FakeEndpoint::Node& n) { n.entry.mtime = {0, 0}; });
  Report same;
  REQUIRE(run_sync(src, dst, fake_options(), same).ok());
  CHECK(same.stats.files_copied == 0);
  CHECK(same.stats.files_unchanged == 3);
}

TEST_CASE("fake: a copy in doubt is made again with --checksum, and keeps its row while its source is gone") {
  TempDir tmp;
  auto journal = Journal::open(tmp.sub("j.sqlite"), "fake", "fake");
  REQUIRE(journal.ok());
  Journal& j = *journal.value();
  FakeEndpoint src, dst;
  src.caps.checksum = dst.caps.checksum = ChecksumType::Adler32;
  src.lists_checksums = dst.lists_checksums = true;
  populate(src);
  Report mirror;
  REQUIRE(run_sync(src, dst, fake_options(), mirror).ok());
  REQUIRE(j.begin_run(false).ok());
  REQUIRE(j.record_failure({"f0", EntryType::File, Error{ErrorKind::IO, "close failed"}}).ok());
  REQUIRE(j.end_run(true).ok());
  SyncOptions o = fake_options();
  o.checksum = true;

  SUBCASE("copied, not compared, though the stored checksums are equal") {
    REQUIRE(j.begin_run(false).ok());
    Report r;
    REQUIRE(run_sync(src, dst, o, r, &j).ok());
    REQUIRE(j.end_run(true).ok());
    CHECK(r.stats.files_copied == 1);
    CHECK(r.stats.checksums_compared == 2);  // the other two files
    CHECK(j.failures().value().empty());
  }
  SUBCASE("the source vanishes before the copy") {
    src.fail("open_read", "f0", Error{ErrorKind::NotFound, "gone"}, 3);
    REQUIRE(j.begin_run(false).ok());
    Report r;
    REQUIRE(run_sync(src, dst, o, r, &j).ok());
    REQUIRE(j.end_run(true).ok());
    CHECK(r.stats.vanished == 1);
    CHECK(r.stats.failures == 0);
    REQUIRE(j.failures().value().size() == 1);
    CHECK(j.failures().value()[0].path == "f0");
    CHECK(dst.get("f0")->entry.mtime == Timespec{0, 0});  // not final
  }
}

TEST_CASE("fake: a retry stops at an empty source with --delete like a plain run") {
  TempDir tmp;
  auto journal = Journal::open(tmp.sub("j.sqlite"), "fake", "fake");
  REQUIRE(journal.ok());
  FakeEndpoint src, dst;
  populate(src);
  Report mirror;
  REQUIRE(run_sync(src, dst, fake_options(), mirror).ok());
  // The source's file system is not mounted: an empty directory.
  for (const char* p : {"f0", "d1"}) src.erase(p);
  SyncOptions o = fake_options();
  o.delete_extra = true;
  Cancellation cancel;
  Report r;
  Engine engine(src, dst, o, r, journal.value().get(), cancel);
  Status s = engine.run({Failure{"f0", EntryType::File, Error{ErrorKind::IO, "x"}}});
  REQUIRE_FALSE(s.ok());
  CHECK(s.error().message.find("is empty") != std::string::npos);
  CHECK(r.stats.deleted == 0);
  CHECK(dst.get("f0"));
}

TEST_CASE("fake: retrying entries whose source directory is gone keeps the rows of what the target has") {
  TempDir tmp;
  auto journal = Journal::open(tmp.sub("j.sqlite"), "fake", "fake");
  REQUIRE(journal.ok());
  Journal& j = *journal.value();
  FakeEndpoint src, dst;
  populate(src);
  Report mirror;
  REQUIRE(run_sync(src, dst, fake_options(), mirror).ok());
  REQUIRE(j.begin_run(false).ok());
  REQUIRE(j.record_failure({"d1/f1", EntryType::File, Error{ErrorKind::IO, "in doubt"}}).ok());
  REQUIRE(j.record_failure({"d1/gone", EntryType::File, Error{ErrorKind::IO, "in doubt"}}).ok());
  REQUIRE(j.end_run(true).ok());
  src.erase("d1");
  REQUIRE(j.begin_run(false).ok());
  Cancellation cancel;
  Report r;
  Engine engine(src, dst, fake_options(), r, &j, cancel);
  REQUIRE(engine.run(j.failures().value()).ok());
  REQUIRE(j.end_run(true).ok());
  CHECK(r.stats.failures == 1);
  REQUIRE(j.failures().value().size() == 1);
  CHECK(j.failures().value()[0].path == "d1/f1");
  CHECK(dst.get("d1/f1"));
}

TEST_CASE("fake: a retry below the top takes no symlink for a directory above it") {
  std::vector<Failure> rows = {Failure{"d1/d2/f2", EntryType::File, Error{ErrorKind::IO, "x"}}};
  Cancellation cancel;
  SUBCASE("on the target") {
    FakeEndpoint src, dst;
    populate(src);
    dst.add_symlink("d1", "/elsewhere");
    dst.add_dir("d1/d2");
    Report r;
    Engine engine(src, dst, fake_options(), r, nullptr, cancel);
    REQUIRE(engine.run(rows).ok());
    CHECK(r.stats.failures == 1);
    CHECK(r.stats.files_copied == 0);
    CHECK_FALSE(dst.get("d1/d2/f2"));
  }
  SUBCASE("on the source") {
    FakeEndpoint src, dst;
    src.add_symlink("d1", "/elsewhere");
    src.add_dir("d1/d2");
    src.add_file("d1/d2/f2", "deep");
    dst.add_dir("d1");
    dst.add_dir("d1/d2");
    Report r;
    Engine engine(src, dst, fake_options(), r, nullptr, cancel);
    REQUIRE(engine.run(rows).ok());
    CHECK(r.stats.failures == 1);
    CHECK(r.stats.files_copied == 0);
  }
}

TEST_CASE("fake: a user's file named like a temporary is left alone on both sides") {
  FakeEndpoint src, dst;
  populate(src);
  src.add_file(".tmp-user", "theirs", 0644, {100, 0});
  dst.add_file(".tmp-user", "stale", 0644, {100, 0});
  SyncOptions o = fake_options();
  o.delete_extra = true;
  Report r;
  REQUIRE(run_sync(src, dst, o, r).ok());
  CHECK(r.stats.source_temps == 1);
  CHECK(r.stats.deleted == 0);
  CHECK(r.stats.stale_temps == 0);
  CHECK(dst.get(".tmp-user")->content == "stale");
}

TEST_CASE("fake: --checksum notices a file that changed since it was listed") {
  FakeEndpoint src, dst;
  populate(src);
  Report mirror;
  REQUIRE(run_sync(src, dst, fake_options(), mirror).ok());
  // Appended after the listing, before the comparison reads it.
  src.hook = [&](std::string_view op, const RelPath& path) {
    if (op == "open_read" && path == "f0")
      src.modify("f0", [](FakeEndpoint::Node& n) {
        n.content = "root file, longer";
        n.entry.size = n.content.size();
      });
  };
  SyncOptions o = fake_options();
  o.checksum = true;
  Report r;
  REQUIRE(run_sync(src, dst, o, r).ok());
  CHECK(r.stats.failures == 1);
  CHECK(r.stats.files_unchanged == 2);
  CHECK(r.stats.checksums_compared == 2);
}

TEST_CASE("fake: a failed sync keeps the top directory a failure and records no directory as done") {
  TempDir tmp;
  auto journal = Journal::open(tmp.sub("j.sqlite"), "fake", "fake");
  REQUIRE(journal.ok());
  Journal& j = *journal.value();
  FakeEndpoint src, dst;
  populate(src);
  SyncOptions o = fake_options();
  o.sync_interval = std::chrono::milliseconds(2);
  dst.fail("flush", "", Error{ErrorKind::IO, "sync failed"});  // the first, periodic one
  dst.hook = [](std::string_view op, const RelPath&) {
    if (op == "commit") std::this_thread::sleep_for(std::chrono::milliseconds(20));
  };
  REQUIRE(j.begin_run(false).ok());
  Report r;
  REQUIRE(run_sync(src, dst, o, r, &j).ok());
  REQUIRE(j.end_run(true).ok());
  CHECK(r.stats.failures == 1);
  REQUIRE(j.failures().value().size() == 1);
  CHECK(j.failures().value()[0].path == "");
  CHECK_FALSE(j.is_done_dir("").value());
  CHECK_FALSE(j.is_done_dir("d1").value());
}

TEST_CASE("fake: a verifying dry run journals every failure, type conflicts too") {
  TempDir tmp;
  auto journal = Journal::open(tmp.sub("j.sqlite"), "fake", "fake");
  REQUIRE(journal.ok());
  Journal& j = *journal.value();
  FakeEndpoint src, dst;
  src.add_file("f", "x");
  dst.add_dir("f");
  SyncOptions o = fake_options();
  o.dry_run = o.checksum = true;
  REQUIRE(j.begin_run(false).ok());
  Report r;
  REQUIRE(run_sync(src, dst, o, r, &j).ok());
  REQUIRE(j.end_run(true).ok());
  CHECK(r.stats.failures == 1);
  REQUIRE(j.failures().value().size() == 1);
  CHECK(j.failures().value()[0].path == "f");
}

TEST_CASE("fake: an upload still under its hidden name after the close is removed and made again") {
  FakeEndpoint src, dst;
  dst.atomic_uploads = true;
  dst.hidden_uploads = 1;
  populate(src);
  Report r;
  REQUIRE(run_sync(src, dst, fake_options(), r).ok());
  CHECK(r.stats.failures == 0);
  CHECK(r.stats.retries == 1);
  CHECK(r.stats.files_copied == 3);
  for (const RelPath& p : dst.paths()) CHECK(p.find(".tmp-") == std::string::npos);
  expect_mirrored(src, dst);
}

TEST_CASE("fake: a failed upload that replaced a file gets mtime 0, one that replaced nothing goes") {
  FakeEndpoint src, dst;
  dst.atomic_uploads = true;
  populate(src);
  Report mirror;
  REQUIRE(run_sync(src, dst, fake_options(), mirror).ok());
  src.modify("f0", [](FakeEndpoint::Node& n) {
    n.content = "root file, v2";
    n.entry.size = n.content.size();
    n.entry.mtime = {3100, 1};
  });
  src.add_file("new", "fresh", 0644, {3200, 1});
  // Every close fails after the upload is in place, which EOS can commit
  // all the same.
  const int attempts = fake_options().retry.attempts;
  for (const char* path : {"f0", "new"})
    dst.fail("close", path, Error{ErrorKind::IO, "close failed"}, attempts);
  Report r;
  REQUIRE(run_sync(src, dst, fake_options(), r).ok());
  CHECK(r.stats.failures == 2);
  // The replacement stays, as a copy that is not final: removing it would
  // take the file's versions on EOS with it. The new file goes.
  REQUIRE(dst.get("f0"));
  CHECK(dst.get("f0")->content == "root file, v2");
  CHECK(dst.get("f0")->entry.mtime == Timespec{0, 0});
  CHECK_FALSE(dst.get("new"));
  CHECK(dst.count("remove") == attempts);  // "new", once per attempt

  // A replacement whose mtime cannot be reset is removed after all.
  dst.fail("close", "f0", Error{ErrorKind::IO, "close failed"}, attempts);
  dst.fail("set_metadata", "f0", Error{ErrorKind::Permission, "locked"}, attempts);
  Report r2;
  REQUIRE(run_sync(src, dst, fake_options(), r2).ok());
  CHECK(r2.stats.failures == 1);
  CHECK_FALSE(dst.get("f0"));

  Report r3;
  REQUIRE(run_sync(src, dst, fake_options(), r3).ok());
  CHECK(r3.stats.failures == 0);
  expect_mirrored(src, dst);
}

TEST_CASE("fake: a copy in doubt since an earlier run stays so when its new copy fails otherwise") {
  TempDir tmp;
  auto journal = Journal::open(tmp.sub("j.sqlite"), "fake", "fake");
  REQUIRE(journal.ok());
  Journal& j = *journal.value();
  FakeEndpoint src, dst;
  // Same size and mtime 0 on both sides: nothing but the doubt tells the
  // corrupt copy from a good one, since mtime 0 cannot mark it as not final.
  src.add_file("f", "good", 0644, {0, 0});
  dst.add_file("f", "evil", 0644, {0, 0});
  REQUIRE(j.begin_run(false).ok());
  REQUIRE(j.record_failure({"f", EntryType::File, Error{ErrorKind::Checksum, "differs"}}).ok());
  REQUIRE(j.end_run(true).ok());
  // The new copy cannot even be opened (and that is not retried).
  dst.fail("open_write", "f", Error{ErrorKind::Permission, "denied"});
  REQUIRE(j.begin_run(false).ok());
  Report r;
  REQUIRE(run_sync(src, dst, fake_options(), r, &j).ok());
  REQUIRE(j.end_run(true).ok());
  CHECK(r.stats.failures == 1);
  auto rows = j.failures().value();
  REQUIRE(rows.size() == 1);
  CHECK(rows[0].error.kind == ErrorKind::IO);
  CHECK(rows[0].error.message.find("in doubt since an earlier run") != std::string::npos);
  // So the next run copies it instead of taking it for unchanged.
  REQUIRE(j.begin_run(false).ok());
  Report r2;
  REQUIRE(run_sync(src, dst, fake_options(), r2, &j).ok());
  REQUIRE(j.end_run(true).ok());
  CHECK(r2.stats.files_copied == 1);
  CHECK(dst.get("f")->content == "good");
  CHECK(j.failures().value().empty());
}

TEST_CASE("fake: an upload that an abort could not discard is taken back by its id") {
  FakeEndpoint src, dst;
  dst.atomic_uploads = true;
  populate(src);
  SUBCASE("one that replaced nothing is removed") {
    dst.committing_aborts = 1;
    dst.fail("write", "f0", Error{ErrorKind::IO, "write failed"});
    Report r;
    REQUIRE(run_sync(src, dst, fake_options(), r).ok());
    CHECK(r.stats.retries == 1);
    CHECK(r.stats.failures == 0);
    CHECK(dst.count("remove") == 1);
  }
  SUBCASE("one that replaced a file gets mtime 0, and is replaced again") {
    Report mirror;
    REQUIRE(run_sync(src, dst, fake_options(), mirror).ok());
    src.modify("f0", [](FakeEndpoint::Node& n) {
      n.content = "root file, v2";
      n.entry.size = n.content.size();
      n.entry.mtime = {3100, 1};
    });
    dst.committing_aborts = 1;
    dst.fail("write", "f0", Error{ErrorKind::IO, "write failed"});
    Report r;
    REQUIRE(run_sync(src, dst, fake_options(), r).ok());
    CHECK(r.stats.retries == 1);
    CHECK(r.stats.failures == 0);
    CHECK(dst.count("remove") == 0);
  }
  expect_mirrored(src, dst);
}

TEST_CASE("fake: a verifying dry run keeps the failures it only would have repaired") {
  TempDir tmp;
  auto journal = Journal::open(tmp.sub("j.sqlite"), "fake", "fake");
  REQUIRE(journal.ok());
  Journal& j = *journal.value();
  FakeEndpoint src, dst;
  populate(src);
  Report mirror;
  REQUIRE(run_sync(src, dst, fake_options(), mirror).ok());
  // A mode that could not be set, an extra that could not be deleted, and a
  // directory whose mtime could not be set.
  dst.modify("f0", [](FakeEndpoint::Node& n) { n.entry.mode = 0600; });
  dst.add_file("extra", "x");
  dst.modify("d1", [](FakeEndpoint::Node& n) { n.entry.mtime = {1, 0}; });
  REQUIRE(j.begin_run(false).ok());
  REQUIRE(j.record_failure({"f0", EntryType::File, Error{ErrorKind::Permission, "denied"}}).ok());
  REQUIRE(j.record_failure({"extra", EntryType::File, Error{ErrorKind::Permission, "denied"}}).ok());
  REQUIRE(j.record_failure({"d1", EntryType::Directory, Error{ErrorKind::Permission, "denied"}}).ok());
  REQUIRE(j.end_run(true).ok());
  SyncOptions o = fake_options();
  o.dry_run = o.checksum = o.delete_extra = true;
  REQUIRE(j.begin_run(false).ok());
  Report r;
  REQUIRE(run_sync(src, dst, o, r, &j).ok());
  REQUIRE(j.end_run(true).ok());
  CHECK(r.stats.metadata_fixed >= 2);  // and the top directory, touched by the extra
  CHECK(r.stats.deleted == 1);
  CHECK(j.failures().value().size() == 3);
  // A run repairs them and clears the rows.
  o.dry_run = false;
  REQUIRE(j.begin_run(false).ok());
  Report r2;
  REQUIRE(run_sync(src, dst, o, r2, &j).ok());
  REQUIRE(j.end_run(true).ok());
  CHECK(j.failures().value().empty());
  expect_mirrored(src, dst);
}

TEST_CASE("fake: retry rows below a directory that is a file on both sides now are dropped") {
  TempDir tmp;
  auto journal = Journal::open(tmp.sub("j.sqlite"), "fake", "fake");
  REQUIRE(journal.ok());
  Journal& j = *journal.value();
  FakeEndpoint src, dst;
  populate(src);
  Report mirror;
  REQUIRE(run_sync(src, dst, fake_options(), mirror).ok());
  REQUIRE(j.begin_run(false).ok());
  REQUIRE(j.record_failure({"d1/d2/f2", EntryType::File, Error{ErrorKind::IO, "x"}}).ok());
  REQUIRE(j.end_run(true).ok());
  for (FakeEndpoint* ep : {&src, &dst}) {
    ep->erase("d1");
    ep->add_file("d1", "now a file");
  }
  REQUIRE(j.begin_run(false).ok());
  Cancellation cancel;
  Report r;
  Engine engine(src, dst, fake_options(), r, &j, cancel);
  REQUIRE(engine.run(j.failures().value()).ok());
  REQUIRE(j.end_run(true).ok());
  CHECK(r.stats.failures == 0);
  CHECK(j.failures().value().empty());
}

TEST_CASE("fake: a retry with --one-file-system does not enter another file system either") {
  TempDir tmp;
  auto journal = Journal::open(tmp.sub("j.sqlite"), "fake", "fake");
  REQUIRE(journal.ok());
  Journal& j = *journal.value();
  FakeEndpoint src, dst;
  populate(src);
  src.add_dir("mnt");
  src.add_file("mnt/data", "elsewhere");
  dst.add_dir("mnt");
  dst.add_file("mnt/old", "kept");
  REQUIRE(j.begin_run(false).ok());
  REQUIRE(j.record_failure({"mnt/data", EntryType::File, Error{ErrorKind::IO, "x"}}).ok());
  REQUIRE(j.end_run(true).ok());
  // Another file system is mounted there since the failure was recorded.
  src.modify("mnt", [](FakeEndpoint::Node& n) { n.entry.id = "other-fs"; });
  SyncOptions o = fake_options();
  o.one_file_system = o.delete_extra = true;
  REQUIRE(j.begin_run(false).ok());
  Cancellation cancel;
  Report r;
  Engine engine(src, dst, o, r, &j, cancel);
  REQUIRE(engine.run(j.failures().value()).ok());
  REQUIRE(j.end_run(true).ok());
  CHECK(r.stats.other_fs_skipped == 1);
  CHECK(r.stats.files_copied == 0);
  CHECK(r.stats.deleted == 0);
  CHECK(dst.get("mnt/old"));
  CHECK_FALSE(dst.get("mnt/data"));
  CHECK(j.failures().value().size() == 1);
}
