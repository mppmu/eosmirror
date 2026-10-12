// SPDX-License-Identifier: GPL-3.0-or-later
//
// Randomized differential tests: random trees and random changes of the
// source (files created, rewritten with or without another size, deleted,
// renamed, given other modes and mtimes; symlinks; type changes), runs with
// injected faults, then a run without them, after which the target must
// equal the source in content and metadata, and a further run must change
// nothing. Each seed is reproducible: a failure names it, and
// EOSMIRROR_SEED=<seed> runs that one alone. EOSMIRROR_SEEDS sets how many
// seeds run (from EOSMIRROR_SEED_BASE, by default the time), and
// EOSMIRROR_VERBOSE=1 shows the runs' logs.
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <random>
#include <string>
#include <vector>

#include "doctest/doctest.h"
#include "eosmirror/engine.hh"
#include "eosmirror/log.hh"
#include "eosmirror/posix_endpoint.hh"
#include "fake_endpoint.hh"
#include "temp_dir.hh"

using namespace eosmirror;
namespace fs = std::filesystem;

namespace {

using Rng = std::mt19937_64;

uint64_t env_number(const char* name, uint64_t fallback) {
  const char* value = std::getenv(name);
  return value && *value ? std::strtoull(value, nullptr, 10) : fallback;
}

// The seeds to run: EOSMIRROR_SEED alone, or EOSMIRROR_SEEDS from a base.
std::vector<uint64_t> seeds(uint64_t count) {
  if (std::getenv("EOSMIRROR_SEED")) return {env_number("EOSMIRROR_SEED", 0)};
  uint64_t base = env_number(
      "EOSMIRROR_SEED_BASE",
      static_cast<uint64_t>(std::chrono::system_clock::now().time_since_epoch().count()) % 1000000000);
  std::vector<uint64_t> result;
  for (uint64_t i = 0; i < env_number("EOSMIRROR_SEEDS", count); ++i) result.push_back(base + i);
  return result;
}

size_t pick(Rng& rng, size_t n) { return std::uniform_int_distribution<size_t>(0, n - 1)(rng); }
bool chance(Rng& rng, double p) { return std::bernoulli_distribution(p)(rng); }

template <class T>
const T& pick_of(Rng& rng, const std::vector<T>& items) {
  return items[pick(rng, items.size())];
}

std::string random_content(Rng& rng, size_t size) {
  std::string s(size, '\0');
  for (auto& c : s) c = static_cast<char>(rng());
  return s;
}

size_t random_size(Rng& rng) {
  if (chance(rng, 0.1)) return 0;
  return chance(rng, 0.2) ? pick(rng, 300000) : pick(rng, 5000);
}

Timespec random_time(Rng& rng) {
  return {1500000000 + static_cast<int64_t>(pick(rng, 100000000)),
          static_cast<int32_t>(pick(rng, 1000000000))};
}

ModeBits random_file_mode(Rng& rng) { return pick_of<ModeBits>(rng, {0600, 0640, 0644, 0444, 0755}); }
ModeBits random_dir_mode(Rng& rng) { return pick_of<ModeBits>(rng, {0700, 0750, 0755, 0775}); }

// A few names, so that later changes reuse them, also for another type.
std::string random_name(Rng& rng) {
  static const std::vector<std::string> kNames = {"a", "b", "c", "d", "e", "f", "g", "h"};
  return pick_of(rng, kNames) + (chance(rng, 0.3) ? std::to_string(pick(rng, 3)) : "");
}

// A source tree that can be changed: the fake endpoint's or a directory's.
class Tree {
 public:
  virtual ~Tree() = default;
  // Every entry (path and type), parents before children.
  virtual std::vector<std::pair<RelPath, EntryType>> entries() = 0;
  virtual void file(const RelPath& path, const std::string& content, ModeBits mode,
                    Timespec mtime) = 0;
  virtual void dir(const RelPath& path, ModeBits mode, Timespec mtime) = 0;
  virtual void link(const RelPath& path, const std::string& target, Timespec mtime) = 0;
  virtual void remove(const RelPath& path) = 0;  // with everything below
  virtual void rename(const RelPath& from, const RelPath& to) = 0;
  virtual void chmod(const RelPath& path, ModeBits mode) = 0;
  virtual void touch(const RelPath& path, Timespec mtime) = 0;
  virtual size_t size(const RelPath& path) = 0;  // of a file
};

class FakeTree : public Tree {
 public:
  explicit FakeTree(FakeEndpoint& ep) : ep_(ep) {}

  std::vector<std::pair<RelPath, EntryType>> entries() override {
    std::vector<std::pair<RelPath, EntryType>> result;
    for (const RelPath& p : ep_.paths()) result.emplace_back(p, ep_.get(p)->entry.type);
    return result;
  }
  void file(const RelPath& path, const std::string& content, ModeBits mode, Timespec mtime) override {
    ep_.add_file(path, content, mode, mtime);
  }
  void dir(const RelPath& path, ModeBits mode, Timespec mtime) override {
    ep_.add_dir(path, mode, mtime);
  }
  void link(const RelPath& path, const std::string& target, Timespec mtime) override {
    ep_.add_symlink(path, target, mtime);
  }
  void remove(const RelPath& path) override { ep_.erase(path); }
  void rename(const RelPath& from, const RelPath& to) override {
    std::vector<std::pair<RelPath, FakeEndpoint::Node>> moved;
    for (const RelPath& p : ep_.paths())
      if (p == from || p.starts_with(from + "/")) moved.emplace_back(to + p.substr(from.size()), *ep_.get(p));
    ep_.erase(from);
    for (auto& [p, node] : moved) {
      const Entry& e = node.entry;
      if (e.type == EntryType::Directory) ep_.add_dir(p, e.mode, e.mtime);
      if (e.type == EntryType::File) ep_.add_file(p, node.content, e.mode, e.mtime);
      if (e.type == EntryType::Symlink) ep_.add_symlink(p, e.link_target, e.mtime);
      ep_.modify(p, [&](FakeEndpoint::Node& n) {
        n.entry.uid = e.uid;
        n.entry.gid = e.gid;
      });
    }
  }
  void chmod(const RelPath& path, ModeBits mode) override {
    ep_.modify(path, [&](FakeEndpoint::Node& n) { n.entry.mode = mode; });
  }
  void touch(const RelPath& path, Timespec mtime) override {
    ep_.modify(path, [&](FakeEndpoint::Node& n) { n.entry.mtime = mtime; });
  }
  size_t size(const RelPath& path) override { return ep_.get(path)->content.size(); }

  // Owners exist only here.
  void chown(const RelPath& path, uint32_t uid) {
    ep_.modify(path, [&](FakeEndpoint::Node& n) { n.entry.uid = n.entry.gid = uid; });
  }

 private:
  FakeEndpoint& ep_;
};

class PosixTree : public Tree {
 public:
  explicit PosixTree(std::string root) : root_(std::move(root)) { fs::create_directories(root_); }

  std::vector<std::pair<RelPath, EntryType>> entries() override {
    std::vector<std::pair<RelPath, EntryType>> result;
    add_entries("", result);
    std::sort(result.begin(), result.end());
    return result;
  }
  void file(const RelPath& path, const std::string& content, ModeBits mode, Timespec mtime) override {
    ::chmod(abs(path).c_str(), 0600);  // if it exists, as read-only
    std::ofstream(abs(path), std::ios::binary | std::ios::trunc) << content;
    ::chmod(abs(path).c_str(), mode);
    touch(path, mtime);
  }
  void dir(const RelPath& path, ModeBits mode, Timespec mtime) override {
    REQUIRE(::mkdir(abs(path).c_str(), mode) == 0);
    ::chmod(abs(path).c_str(), mode);
    touch(path, mtime);
  }
  void link(const RelPath& path, const std::string& target, Timespec mtime) override {
    REQUIRE(::symlink(target.c_str(), abs(path).c_str()) == 0);
    touch(path, mtime);
  }
  void remove(const RelPath& path) override { fs::remove_all(abs(path)); }
  void rename(const RelPath& from, const RelPath& to) override {
    REQUIRE(::rename(abs(from).c_str(), abs(to).c_str()) == 0);
  }
  void chmod(const RelPath& path, ModeBits mode) override { ::chmod(abs(path).c_str(), mode); }
  void touch(const RelPath& path, Timespec mtime) override {
    struct timespec times[2] = {{0, UTIME_OMIT}, {mtime.sec, mtime.nsec}};
    REQUIRE(utimensat(AT_FDCWD, abs(path).c_str(), times, AT_SYMLINK_NOFOLLOW) == 0);
  }
  size_t size(const RelPath& path) override { return fs::file_size(abs(path)); }

 private:
  std::string abs(const RelPath& path) const { return path.empty() ? root_ : root_ + "/" + path; }

  // Symlinks are not followed (they can point to themselves).
  void add_entries(const RelPath& dir, std::vector<std::pair<RelPath, EntryType>>& result) {
    for (const fs::directory_entry& entry : fs::directory_iterator(abs(dir))) {
      RelPath path = join(dir, entry.path().filename().string());
      fs::file_type type = entry.symlink_status().type();
      if (type == fs::file_type::symlink) {
        result.emplace_back(path, EntryType::Symlink);
      } else if (type == fs::file_type::directory) {
        result.emplace_back(path, EntryType::Directory);
        add_entries(path, result);
      } else {
        result.emplace_back(path, EntryType::File);
      }
    }
  }

  std::string root_;
};

std::vector<RelPath> of_type(Tree& tree, std::initializer_list<EntryType> types) {
  std::vector<RelPath> result;
  for (const auto& [p, t] : tree.entries())
    if (std::find(types.begin(), types.end(), t) != types.end()) result.push_back(p);
  return result;
}

// A new path in a random directory, which may be taken by an entry.
RelPath random_path(Rng& rng, Tree& tree) {
  std::vector<RelPath> dirs = of_type(tree, {EntryType::Directory});
  dirs.push_back("");
  RelPath dir = pick_of(rng, dirs);
  return join(dir, random_name(rng));
}

// Makes room for a new entry at path, as a user replacing it would.
void make_room(Tree& tree, const RelPath& path) {
  for (const auto& [p, t] : tree.entries())
    if (p == path) return tree.remove(path);
}

void random_change(Rng& rng, Tree& tree) {
  std::vector<RelPath> files = of_type(tree, {EntryType::File});
  std::vector<RelPath> all = of_type(tree, {EntryType::File, EntryType::Directory, EntryType::Symlink});
  switch (pick(rng, 10)) {
    case 0:  // a new file, perhaps in place of something else
    case 1: {
      RelPath path = random_path(rng, tree);
      make_room(tree, path);
      tree.file(path, random_content(rng, random_size(rng)), random_file_mode(rng), random_time(rng));
      break;
    }
    case 2: {  // a new directory with a few files
      RelPath path = random_path(rng, tree);
      make_room(tree, path);
      tree.dir(path, random_dir_mode(rng), random_time(rng));
      for (size_t i = 0, n = pick(rng, 4); i < n; ++i)
        tree.file(path + "/f" + std::to_string(i), random_content(rng, random_size(rng)),
                  random_file_mode(rng), random_time(rng));
      tree.touch(path, random_time(rng));
      break;
    }
    case 3:  // rewritten with its size and another mtime
      if (!files.empty()) {
        RelPath path = pick_of(rng, files);
        tree.file(path, random_content(rng, tree.size(path)), random_file_mode(rng),
                  random_time(rng));
      }
      break;
    case 4:  // rewritten with another size
      if (!files.empty()) {
        RelPath path = pick_of(rng, files);
        tree.file(path, random_content(rng, random_size(rng) + 1), random_file_mode(rng),
                  random_time(rng));
      }
      break;
    case 5:  // deleted, directories with everything below
      if (!all.empty()) tree.remove(pick_of(rng, all));
      break;
    case 6:  // renamed, also over another entry
      if (!all.empty()) {
        RelPath from = pick_of(rng, all);
        RelPath to = random_path(rng, tree);
        if (to == from || to.starts_with(from + "/") || from.starts_with(to + "/")) break;
        make_room(tree, to);
        tree.rename(from, to);
      }
      break;
    case 7:  // another mode
      if (!all.empty()) {
        RelPath path = pick_of(rng, all);
        for (const auto& [p, t] : tree.entries()) {
          if (p != path || t == EntryType::Symlink) continue;
          tree.chmod(path, t == EntryType::Directory ? random_dir_mode(rng) : random_file_mode(rng));
        }
      }
      break;
    case 8:  // another mtime
      if (!all.empty()) tree.touch(pick_of(rng, all), random_time(rng));
      break;
    case 9: {  // a symlink, new or replacing an entry
      RelPath path = random_path(rng, tree);
      make_room(tree, path);
      tree.link(path, chance(rng, 0.5) ? random_name(rng) : "../" + random_name(rng) + "/x",
                random_time(rng));
      break;
    }
  }
}

void random_tree(Rng& rng, Tree& tree) {
  for (size_t i = 0, n = 10 + pick(rng, 30); i < n; ++i) random_change(rng, tree);
}

// Random changes, which leave something in the tree: a run stops at an
// empty source rather than empty the target.
void random_changes(Rng& rng, Tree& tree) {
  for (size_t i = 0, n = 1 + pick(rng, 12); i < n; ++i) random_change(rng, tree);
  if (tree.entries().empty())
    tree.file("a", random_content(rng, random_size(rng)), random_file_mode(rng), random_time(rng));
}

// The entries below dir as listed by an endpoint, by name, without the
// target's temporaries.
std::map<std::string, Entry> listing(Endpoint& ep, const RelPath& dir) {
  auto listed = ep.list(dir);
  REQUIRE(listed.ok());
  REQUIRE(listed.value().skipped == 0);
  std::map<std::string, Entry> result;
  for (Entry& e : listed.value().entries)
    if (!ep.is_temporary(e)) result.emplace(e.name, std::move(e));
  return result;
}

std::string read_all(Endpoint& ep, const RelPath& path, uint64_t size) {
  auto reader = ep.open_read(path);
  REQUIRE(reader.ok());
  std::string data(size, '\0');
  auto n = reader.value()->read(0, {reinterpret_cast<std::byte*>(data.data()), data.size()});
  REQUIRE(n.ok());
  data.resize(n.value());
  return data;
}

// Whether the target equals the source below dir: entries, types, contents,
// modes, mtimes, owners where compared, link targets.
void compare(Endpoint& src, Endpoint& dst, const RelPath& dir, bool owners) {
  auto s = listing(src, dir);
  auto d = listing(dst, dir);
  for (auto& [name, e] : s) {
    if (e.type == EntryType::Other) continue;
    RelPath path = join(dir, name);
    INFO("entry ", path);
    auto it = d.find(name);
    REQUIRE(it != d.end());
    const Entry& t = it->second;
    REQUIRE(t.type == e.type);
    CHECK(t.mtime == e.mtime);
    if (owners) CHECK(t.uid == e.uid);
    if (owners) CHECK(t.gid == e.gid);
    if (e.type != EntryType::Symlink) CHECK(t.mode == e.mode);
    if (e.type == EntryType::Symlink) CHECK(t.link_target == e.link_target);
    if (e.type == EntryType::File) {
      CHECK(t.size == e.size);
      CHECK(read_all(src, path, e.size) == read_all(dst, path, t.size));
    }
    if (e.type == EntryType::Directory) compare(src, dst, path, owners);
    d.erase(it);
  }
  INFO("extra entries in ", dir.empty() ? "." : dir);
  CHECK(d.empty());
  if (dir.empty()) CHECK(dst.stat("").value().mtime == src.stat("").value().mtime);
}

// That no file on the target passes for the source's (same size and mtime)
// with other content: the next run would keep it.
void no_wrong_files(Endpoint& src, Endpoint& dst, const RelPath& dir) {
  auto s = listing(src, dir);
  auto listed = dst.list(dir);
  if (!listed.ok()) return;
  for (const Entry& t : listed.value().entries) {
    auto it = s.find(t.name);
    if (it == s.end() || it->second.type != t.type) continue;
    RelPath path = join(dir, t.name);
    INFO("entry ", path);
    if (t.type == EntryType::Directory) no_wrong_files(src, dst, path);
    if (t.type == EntryType::File && t.size == it->second.size && t.mtime == it->second.mtime)
      CHECK(read_all(src, path, t.size) == read_all(dst, path, t.size));
  }
}

SyncOptions random_options(Rng& rng) {
  SyncOptions o;
  o.checkers = 1 + static_cast<int>(pick(rng, 4));
  o.min_transfers = o.max_transfers = 1 + static_cast<int>(pick(rng, 4));
  o.buffer_size = 4096 << pick(rng, 5);
  o.retry.attempts = 3;
  o.retry.initial_delay = std::chrono::milliseconds(1);
  o.retry.factor = 1;
  o.delete_extra = true;
  o.delete_emptied = true;
  o.max_consecutive_failures = 0;
  return o;
}

// A run with the journal. Faults can end a run early, such as one in the
// top directory.
Status sync(Endpoint& src, Endpoint& dst, const SyncOptions& options, Journal& journal,
            Report& report, bool retry_failed = false) {
  Cancellation cancel;
  REQUIRE(journal.begin_run(false).ok());
  Engine engine(src, dst, options, report, &journal, cancel);
  Status s = retry_failed ? engine.run(journal.failures().value()) : engine.run();
  REQUIRE(journal.end_run(s.ok()).ok());
  return s;
}

// The run completed.
void completed(const Status& s) {
  CHECK_MESSAGE(s.ok(), (s.ok() ? "" : s.error().describe()));
}

// Faults for a run, on paths of the source's tree on either side.
void random_faults(Rng& rng, FakeEndpoint& src, FakeEndpoint& dst) {
  std::vector<RelPath> paths = src.paths();
  paths.push_back("");
  static const std::vector<std::string> kSourceOps = {"list", "open_read", "read", "stat"};
  static const std::vector<std::string> kTargetOps = {
      "list",   "mkdir",        "open_write", "write",    "commit", "close",
      "remove", "set_metadata", "symlink",    "checksum", "stat"};
  static const std::vector<ErrorKind> kKinds = {ErrorKind::IO,         ErrorKind::IO,
                                                ErrorKind::Timeout,    ErrorKind::Permission,
                                                ErrorKind::NoSpace,    ErrorKind::Checksum,
                                                ErrorKind::Other};
  for (size_t i = 0, n = pick(rng, 8); i < n; ++i) {
    bool on_source = chance(rng, 0.3);
    const std::string& op = pick_of(rng, on_source ? kSourceOps : kTargetOps);
    Error error{pick_of(rng, kKinds), "injected " + op + " fault"};
    int times = 1 + static_cast<int>(pick(rng, 4));
    (on_source ? src : dst).fail(op, pick_of(rng, paths), error, times);
  }
  // Entries that a listing cannot list safely.
  if (chance(rng, 0.2) && !paths.empty()) src.unlistable.insert(pick_of(rng, paths));
  if (chance(rng, 0.2) && !paths.empty()) dst.unlistable.insert(pick_of(rng, paths));
}

}  // namespace

TEST_CASE("differential: fake endpoints with faults") {
  log::set_level(std::getenv("EOSMIRROR_VERBOSE") ? LogLevel::Debug : LogLevel::Error);
  auto run_seeds = seeds(2000);
  MESSAGE("seeds ", run_seeds.front(), " to ", run_seeds.back(), " (EOSMIRROR_SEED=<seed> repeats one)");
  for (uint64_t seed : run_seeds) {
    INFO("seed ", seed);
    Rng rng(seed);
    TempDir tmp;
    auto journal = Journal::open(tmp.sub("j.sqlite"), "fake-src", "fake-dst");
    REQUIRE(journal.ok());
    FakeEndpoint src("fake-src"), dst("fake-dst");
    dst.atomic_uploads = chance(rng, 0.5);
    if (chance(rng, 0.5)) dst.caps.checksum = ChecksumType::Adler32;
    FakeTree tree(src);
    random_tree(rng, tree);
    for (int round = 0; round < 5; ++round) {
      INFO("round ", round);
      random_changes(rng, tree);
      if (chance(rng, 0.2)) {
        auto files = of_type(tree, {EntryType::File, EntryType::Directory});
        if (!files.empty()) tree.chown(pick_of(rng, files), 1000 + static_cast<uint32_t>(pick(rng, 3)));
      }

      random_faults(rng, src, dst);
      SyncOptions faulty = random_options(rng);
      faulty.delete_extra = chance(rng, 0.7);
      faulty.delete_emptied = chance(rng, 0.5);
      Report with_faults;
      (void)sync(src, dst, faulty, *journal.value(), with_faults);  // which faults may end
      src.clear_faults();
      dst.clear_faults();
      src.unlistable.clear();
      dst.unlistable.clear();
      no_wrong_files(src, dst, "");

      SyncOptions clean = random_options(rng);
      Report retried, r, again;
      if (chance(rng, 0.5))
        completed(sync(src, dst, clean, *journal.value(), retried, /*retry_failed=*/true));
      completed(sync(src, dst, clean, *journal.value(), r));
      CHECK(r.stats.failures == 0);
      compare(src, dst, "", true);
      CHECK(journal.value()->failures().value().empty());

      completed(sync(src, dst, clean, *journal.value(), again));
      CHECK(again.stats.files_copied == 0);
      CHECK(again.stats.symlinks_created == 0);
      CHECK(again.stats.metadata_fixed == 0);
      CHECK(again.stats.deleted == 0);
    }
  }
}

TEST_CASE("differential: local file systems") {
  log::set_level(std::getenv("EOSMIRROR_VERBOSE") ? LogLevel::Debug : LogLevel::Error);
  auto run_seeds = seeds(2000);
  MESSAGE("seeds ", run_seeds.front(), " to ", run_seeds.back(), " (EOSMIRROR_SEED=<seed> repeats one)");
  for (uint64_t seed : run_seeds) {
    INFO("seed ", seed);
    Rng rng(seed);
    TempDir tmp;
    PosixTree tree(tmp.sub("src"));
    random_tree(rng, tree);
    tree.touch("", random_time(rng));
    PosixEndpoint src(tmp.sub("src"));
    PosixEndpoint dst(tmp.sub("dst"));
    auto journal = Journal::open(tmp.sub("j.sqlite"), src.describe(), dst.describe());
    REQUIRE(journal.ok());
    for (int round = 0; round < 4; ++round) {
      INFO("round ", round);
      random_changes(rng, tree);
      SyncOptions options = random_options(rng);
      options.preserve_owner = is_root();
      Report r, again;
      completed(sync(src, dst, options, *journal.value(), r));
      CHECK(r.stats.failures == 0);
      compare(src, dst, "", is_root());
      completed(sync(src, dst, options, *journal.value(), again));
      CHECK(again.stats.files_copied == 0);
      CHECK(again.stats.metadata_fixed == 0);
      CHECK(again.stats.deleted == 0);
    }
  }
}
