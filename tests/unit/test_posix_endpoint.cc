// SPDX-License-Identifier: GPL-3.0-or-later
#include "eosmirror/posix_endpoint.hh"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <filesystem>

#include "doctest/doctest.h"
#include "eosmirror/endpoints.hh"
#include "fake_endpoint.hh"
#include "no_new_threads.hh"
#include "temp_dir.hh"

using namespace eosmirror;
namespace fs = std::filesystem;

namespace {

std::span<const std::byte> bytes(std::string_view s) {
  return {reinterpret_cast<const std::byte*>(s.data()), s.size()};
}

const Entry* find(const std::vector<Entry>& entries, std::string_view name) {
  auto it = std::find_if(entries.begin(), entries.end(), [&](const Entry& e) { return e.name == name; });
  return it == entries.end() ? nullptr : &*it;
}

Timespec set_mtime(const std::string& path, int64_t sec, int32_t nsec) {
  struct timespec times[2] = {{sec, nsec}, {sec, nsec}};
  REQUIRE(utimensat(AT_FDCWD, path.c_str(), times, AT_SYMLINK_NOFOLLOW) == 0);
  return {sec, nsec};
}

}  // namespace

TEST_CASE("posix listing reports every entry type with metadata") {
  TempDir tmp;
  tmp.write_file("src/file.txt", "hello", 0640);
  fs::create_directories(tmp.sub("src/sub"));
  chmod(tmp.sub("src/sub").c_str(), 02750);
  REQUIRE(symlink("file.txt", tmp.sub("src/link").c_str()) == 0);
  REQUIRE(mkfifo(tmp.sub("src/fifo").c_str(), 0600) == 0);
  Timespec t = set_mtime(tmp.sub("src/file.txt"), 1700000000, 123456789);

  PosixEndpoint ep(tmp.sub("src"));
  auto listed = ep.list("");
  REQUIRE(listed.ok());
  const auto& entries = listed.value().entries;
  CHECK(entries.size() == 4);

  const Entry* file = find(entries, "file.txt");
  REQUIRE(file);
  CHECK(file->type == EntryType::File);
  CHECK(file->size == 5);
  CHECK(file->mode == 0640);
  CHECK(file->mtime == t);
  CHECK(file->uid == geteuid());

  const Entry* sub = find(entries, "sub");
  REQUIRE(sub);
  CHECK(sub->type == EntryType::Directory);
  CHECK(sub->mode == 02750);
  // Directories carry their identity, however they are reached.
  CHECK_FALSE(sub->id.empty());
  CHECK(sub->id == PosixEndpoint(tmp.path()).stat("src/sub").value().id);
  CHECK(sub->id != ep.stat("").value().id);

  const Entry* link = find(entries, "link");
  REQUIRE(link);
  CHECK(link->type == EntryType::Symlink);
  CHECK(link->link_target == "file.txt");

  const Entry* fifo = find(entries, "fifo");
  REQUIRE(fifo);
  CHECK(fifo->type == EntryType::Other);

  auto st = ep.stat("link");
  REQUIRE(st.ok());
  CHECK(st.value().type == EntryType::Symlink);
  CHECK(st.value().link_target == "file.txt");
  CHECK(st.value().name == "link");

  auto root = ep.stat("");
  REQUIRE(root.ok());
  CHECK(root.value().type == EntryType::Directory);

  auto missing = ep.stat("nope");
  REQUIRE_FALSE(missing.ok());
  CHECK(missing.error().kind == ErrorKind::NotFound);

  auto not_dir = ep.list("file.txt");
  REQUIRE_FALSE(not_dir.ok());
  CHECK(not_dir.error().kind == ErrorKind::NotADirectory);
}

TEST_CASE("posix writer commits atomically with metadata") {
  TempDir tmp;
  PosixEndpoint ep(tmp.path());

  CommitSpec spec;
  spec.size = 11;
  spec.metadata.mode = 0600;
  spec.metadata.mtime = {1600000000, 42};
  spec.metadata.uid = geteuid();
  spec.metadata.gid = getegid();

  auto writer = ep.open_write("out.bin", spec);
  REQUIRE(writer.ok());
  Hasher hasher(ChecksumType::Adler32);
  REQUIRE(writer.value()->write(0, bytes("hello ")).ok());
  hasher.update(bytes("hello "));

  // Nothing is visible under the final name before the commit, and the
  // temporary is recognized as ours.
  CHECK_FALSE(fs::exists(tmp.sub("out.bin")));
  auto during = ep.list("");
  REQUIRE(during.ok());
  REQUIRE(during.value().entries.size() == 1);
  CHECK(ep.is_temporary(during.value().entries[0]));
  Entry named_so = during.value().entries[0];
  named_so.type = EntryType::Directory;
  CHECK_FALSE(ep.is_temporary(named_so));
  named_so.type = EntryType::File;
  named_so.name = "out.bin";
  CHECK_FALSE(ep.is_temporary(named_so));

  REQUIRE(writer.value()->write(6, bytes("world")).ok());
  hasher.update(bytes("world"));
  spec.checksum = hasher.finish();
  auto committed = writer.value()->commit(spec);
  REQUIRE(committed.ok());
  CHECK_FALSE(committed.value().verified);  // no readback was asked for

  CHECK(tmp.read_file("out.bin") == "hello world");
  auto st = ep.stat("out.bin");
  REQUIRE(st.ok());
  CHECK(st.value().mode == 0600);
  // The mtime once a sync has made the copy durable.
  CHECK(st.value().mtime == Timespec{0, 0});
  REQUIRE(ep.flush().ok());
  CHECK(ep.stat("out.bin").value().mtime == Timespec{1600000000, 42});
  CHECK(ep.list("").value().entries.size() == 1);  // the temporary is gone

  SUBCASE("a size mismatch is rejected and leaves no trace") {
    auto w = ep.open_write("short.bin", spec);
    REQUIRE(w.ok());
    REQUIRE(w.value()->write(0, bytes("abc")).ok());
    auto s = w.value()->commit(spec);
    REQUIRE_FALSE(s.ok());
    CHECK(s.error().kind == ErrorKind::Changed);
    CHECK_FALSE(fs::exists(tmp.sub("short.bin")));
    CHECK(ep.list("").value().entries.size() == 1);
  }

  SUBCASE("non-sequential writes are rejected") {
    auto w = ep.open_write("gap.bin", spec);
    REQUIRE(w.ok());
    CHECK_FALSE(w.value()->write(5, bytes("x")).ok());
    w.value()->abort();
    CHECK(ep.list("").value().entries.size() == 1);
  }

  SUBCASE("a dropped writer cleans up") {
    {
      auto w = ep.open_write("dropped.bin", spec);
      REQUIRE(w.ok());
      REQUIRE(w.value()->write(0, bytes("x")).ok());
    }
    CHECK(ep.list("").value().entries.size() == 1);
  }

  SUBCASE("commit replaces an existing file") {
    auto w = ep.open_write("out.bin", spec);
    REQUIRE(w.ok());
    REQUIRE(w.value()->write(0, bytes("HELLO WORLD")).ok());
    REQUIRE(w.value()->commit(spec).ok());
    CHECK(tmp.read_file("out.bin") == "HELLO WORLD");
  }
}

TEST_CASE("posix copies get their mtimes once a sync has made them durable") {
  TempDir tmp;
  CommitSpec spec;
  spec.size = 4;
  spec.fields = MetaFields::Mode | MetaFields::Mtime;
  spec.metadata.mode = 0640;
  spec.metadata.mtime = {1600000000, 7};
  auto copy = [&](PosixEndpoint& ep, const RelPath& path) {
    auto w = ep.open_write(path, spec);
    REQUIRE(w.ok());
    REQUIRE(w.value()->write(0, bytes("data")).ok());
    REQUIRE(w.value()->commit(spec).ok());
    return ep.stat(path).value().mtime;
  };

  PosixEndpoint ep(tmp.path());
  CHECK(copy(ep, "a") == Timespec{0, 0});
  CHECK(ep.stat("a").value().mode == 0640);
  REQUIRE(ep.flush().ok());
  CHECK(ep.stat("a").value().mtime == spec.metadata.mtime);
  // A copy after a flush waits for the next.
  CHECK(copy(ep, "a") == Timespec{0, 0});
  REQUIRE(ep.flush().ok());
  CHECK(ep.stat("a").value().mtime == spec.metadata.mtime);

  // Copies left when the endpoint goes get theirs too.
  {
    PosixEndpoint once(tmp.path());
    CHECK(copy(once, "b") == Timespec{0, 0});
  }
  CHECK(PosixEndpoint(tmp.path()).stat("b").value().mtime == spec.metadata.mtime);

  // Without syncs, at once.
  PosixOptions unsynced;
  unsynced.sync = false;
  PosixEndpoint plain(tmp.path(), unsynced);
  CHECK(copy(plain, "c") == spec.metadata.mtime);

  // A source mtime of 0 could not be told from the provisional one: such a
  // copy is synced at once and gets its mtime with it.
  spec.metadata.mtime = {0, 7};
  CHECK(copy(ep, "d") == Timespec{0, 7});
}

TEST_CASE("posix copies get their mtimes also where no threads can be started for that") {
  TempDir tmp;
  PosixEndpoint ep(tmp.path());
  CommitSpec spec;
  spec.size = 1;
  spec.fields = MetaFields::Mtime;
  spec.metadata.mtime = {1600000000, 7};
  constexpr int kCopies = 1001;  // enough for two threads
  for (int i = 0; i < kCopies; ++i) {
    auto w = ep.open_write(std::to_string(i), spec);
    REQUIRE(w.ok());
    REQUIRE(w.value()->write(0, bytes("x")).ok());
    REQUIRE(w.value()->commit(spec).ok());
  }
  Status flushed;
  {
    NoNewThreads limit;
    flushed = ep.flush();
  }
  CHECK(flushed.ok());
  int with_mtime = 0;
  for (int i = 0; i < kCopies; ++i)
    with_mtime += ep.stat(std::to_string(i)).value().mtime == spec.metadata.mtime;
  CHECK(with_mtime == kCopies);
}

TEST_CASE("posix readback verification catches corruption") {
  TempDir tmp;
  PosixEndpoint ep(tmp.path(), {.verify_readback = true});
  CommitSpec spec;
  spec.size = 3;
  spec.metadata.mode = 0644;
  spec.fields = MetaFields::Mode;

  auto w = ep.open_write("f", spec);
  REQUIRE(w.ok());
  REQUIRE(w.value()->write(0, bytes("abc")).ok());
  spec.checksum = {ChecksumType::Adler32, "deadbeef"};
  auto s = w.value()->commit(spec);
  REQUIRE_FALSE(s.ok());
  CHECK(s.error().kind == ErrorKind::Checksum);
  CHECK_FALSE(fs::exists(tmp.sub("f")));

  auto w2 = ep.open_write("f", spec);
  REQUIRE(w2.ok());
  REQUIRE(w2.value()->write(0, bytes("abc")).ok());
  Hasher h(ChecksumType::Adler32);
  h.update(bytes("abc"));
  spec.checksum = h.finish();
  CHECK(w2.value()->commit(spec).ok());
  CHECK(tmp.read_file("f") == "abc");
}

TEST_CASE("posix reader streams and reports the current stat") {
  TempDir tmp;
  tmp.write_file("data", "0123456789");
  PosixEndpoint ep(tmp.path());

  auto r = ep.open_read("data");
  REQUIRE(r.ok());
  std::byte buf[4];
  auto n = r.value()->read(0, buf);
  REQUIRE(n.ok());
  CHECK(n.value() == 4);
  n = r.value()->read(8, buf);
  REQUIRE(n.ok());
  CHECK(n.value() == 2);
  CHECK(static_cast<char>(buf[1]) == '9');
  n = r.value()->read(10, buf);
  REQUIRE(n.ok());
  CHECK(n.value() == 0);

  auto st = r.value()->stat();
  REQUIRE(st.ok());
  CHECK(st.value().size == 10);

  REQUIRE(symlink("data", tmp.sub("ln").c_str()) == 0);
  auto through_link = ep.open_read("ln");
  CHECK_FALSE(through_link.ok());  // symlinks are never followed

  // A FIFO in place of a file must not block the open.
  REQUIRE(mkfifo(tmp.sub("fifo").c_str(), 0600) == 0);
  auto fifo = ep.open_read("fifo");
  REQUIRE_FALSE(fifo.ok());
  CHECK(fifo.error().kind == ErrorKind::Changed);

  ep.probe_target();
  CHECK(ep.capabilities().mtime_resolution == 1);  // tmpfs and ext4 keep nanoseconds
}

TEST_CASE("posix roots have their canonical path, also before they exist") {
  TempDir tmp;
  fs::create_directories(tmp.sub("real/dir"));
  REQUIRE(symlink("real", tmp.sub("link").c_str()) == 0);
  std::string real = fs::canonical(tmp.sub("real")).string();
  CHECK(PosixEndpoint(tmp.sub("link/dir/")).describe() == real + "/dir");
  CHECK(PosixEndpoint(tmp.sub("link/./new/../dir")).describe() == real + "/dir");
  CHECK(PosixEndpoint(tmp.sub("link/./missing/")).describe() == real + "/missing");
  CHECK(PosixEndpoint(tmp.sub("link/missing/deeper")).describe() == real + "/missing/deeper");
  CHECK(PosixEndpoint("/").describe() == "/");
  // ".." behind a missing component exposes the symlink before it.
  CHECK(PosixEndpoint(tmp.sub("missing/../link/dir")).describe() == real + "/dir");
  CHECK(PosixEndpoint(tmp.sub("link/dir/missing/../../missing2/../")).describe() == real);
}

TEST_CASE("trees that overlap are found by whole path components") {
  auto at = [](std::string server, std::string path) { return Location{server, path}; };
  CHECK(contains(at("", "/data"), at("", "/data")));
  CHECK(contains(at("", "/data"), at("", "/data/proj")));
  CHECK(contains(at("", "/"), at("", "/data")));
  CHECK_FALSE(contains(at("", "/data/proj"), at("", "/data")));
  CHECK_FALSE(contains(at("", "/data/proj"), at("", "/data/project")));
  CHECK_FALSE(contains(at("h:1094", "/data"), at("", "/data/proj")));
  CHECK_FALSE(contains(at("h:1094", "/data"), at("g:1094", "/data")));
  CHECK(contains(at("h:1094", "/eos/a"), at("h:1094", "/eos/a/b")));

  TempDir tmp;
  tmp.write_file("data/proj/f", "x");
  tmp.write_file("data/other", "y");
  REQUIRE(symlink("data/proj", tmp.sub("link").c_str()) == 0);
  auto disjoint = [&](const std::string& src, const std::string& dst) {
    return check_disjoint(PosixEndpoint(tmp.sub(src)), PosixEndpoint(tmp.sub(dst)));
  };
  CHECK(disjoint("data/proj", "data/project").ok());
  CHECK(disjoint("data/proj", "mirror/proj").ok());
  // A target around the source, inside it (not there yet), or the same tree
  // under another name.
  auto around = disjoint("data/proj", "data");
  REQUIRE_FALSE(around.ok());
  CHECK(around.error().message.find("source " + fs::canonical(tmp.sub("data/proj")).string() +
                                    " is inside the target") != std::string::npos);
  auto inside = disjoint("data/proj", "data/proj/mirror/new");
  REQUIRE_FALSE(inside.ok());
  CHECK(inside.error().message.find("target") != std::string::npos);
  CHECK_FALSE(fs::exists(tmp.sub("data/proj/mirror")));
  CHECK_FALSE(disjoint("link", "data/proj").ok());
  CHECK_FALSE(disjoint("link/../proj/", "data/proj/./x/../").ok());
  CHECK_FALSE(disjoint("data/proj", "link/sub").ok());

  // Servers are the same by name, or by port and address.
  FakeEndpoint by_name("localhost:1094"), by_address("127.0.0.1:1094"), other_port("localhost:1095");
  CHECK_FALSE(check_disjoint(by_name, by_address).ok());
  CHECK(check_disjoint(by_name, other_port).ok());
}

TEST_CASE("posix mkdir, symlink, metadata and remove") {
  TempDir tmp;
  PosixEndpoint ep(tmp.path());

  REQUIRE(ep.mkdir("d", 0555).ok());
  CHECK(ep.mkdir("d", 0555).error().kind == ErrorKind::Exists);
  // Created with the owner's rwx bits added, entries can be created inside.
  CHECK((ep.stat("d").value().mode & 0700) == 0700);

  REQUIRE(ep.symlink("d/l", "target1").ok());
  CHECK(ep.stat("d/l").value().link_target == "target1");
  REQUIRE(ep.symlink("d/l", "target2").ok());  // replaced atomically
  CHECK(ep.stat("d/l").value().link_target == "target2");
  CHECK(ep.list("d").value().entries.size() == 1);

  Entry md;
  md.type = EntryType::Directory;
  md.mode = 0555;
  md.mtime = {1500000000, 7};
  md.uid = geteuid();
  md.gid = getegid();
  REQUIRE(ep.set_metadata("d", md, MetaFields::All).ok());
  auto d = ep.stat("d").value();
  CHECK(d.mode == 0555);
  CHECK(d.mtime == Timespec{1500000000, 7});

  Entry lmd;
  lmd.type = EntryType::Symlink;
  lmd.mode = 0777;
  lmd.mtime = {1400000000, 9};
  REQUIRE(ep.set_metadata("d/l", lmd, MetaFields::Mode | MetaFields::Mtime).ok());
  CHECK(ep.stat("d/l").value().mtime == Timespec{1400000000, 9});
  CHECK(ep.stat("d").value().mtime == Timespec{1500000000, 7});  // untouched

  CHECK(ep.remove("d", EntryType::Directory).error().kind == ErrorKind::NotEmpty);
  REQUIRE(ep.remove("d/l", EntryType::Symlink).ok());
  REQUIRE(ep.remove("d", EntryType::Directory).ok());
  CHECK(ep.stat("d").error().kind == ErrorKind::NotFound);

  if (is_root()) {
    tmp.write_file("owned", "x");
    Entry o;
    o.type = EntryType::File;
    o.uid = 12345;
    o.gid = 23456;
    REQUIRE(ep.set_metadata("owned", o, MetaFields::Owner).ok());
    auto st = ep.stat("owned").value();
    CHECK(st.uid == 12345);
    CHECK(st.gid == 23456);
    CHECK(ep.capabilities().can_set_owner);
  } else {
    CHECK_FALSE(ep.capabilities().can_set_owner);
  }
}

TEST_CASE("posix directories on other file systems than the root are told by their device") {
  TempDir tmp;
  fs::create_directories(tmp.sub("root/sub"));
  PosixEndpoint ep(tmp.sub("root"));
  CHECK_FALSE(ep.other_file_system(ep.stat("sub").value()));
  // /proc is a file system of its own.
  PosixEndpoint top("/");
  CHECK(top.other_file_system(top.stat("proc").value()));
  CHECK_FALSE(top.other_file_system(top.stat("").value()));
}

TEST_CASE("the file system type of a path comes from the longest mount point above it") {
  std::string table =
      "22 1 0:21 / / rw,relatime shared:1 - ext4 /dev/sda1 rw\n"
      "23 22 0:22 / /eos rw shared:2 - tmpfs tmpfs rw\n"
      "24 23 0:45 / /eos/user rw,nosuid,nodev,relatime shared:3 - fuse.eosxd eosuser rw,user_id=0\n"
      "25 22 0:46 / /data/my\\040disk rw master:1 propagate_from:2 - xfs /dev/sdb rw\n"
      "26 22 0:47 / /eos/user-old rw - nfs4 server:/x rw\n"
      "garbage line\n";
  CHECK(mount_type(table, "/eos/user/o/oschulz/data") == "fuse.eosxd");
  CHECK(mount_type(table, "/eos/user") == "fuse.eosxd");
  CHECK(mount_type(table, "/eos/user-old/x") == "nfs4");
  CHECK(mount_type(table, "/eos/project") == "tmpfs");
  CHECK(mount_type(table, "/data/my disk/x") == "xfs");
  CHECK(mount_type(table, "/data/my") == "ext4");
  CHECK(mount_type(table, "/home") == "ext4");
  CHECK_FALSE(mount_type("", "/home"));
  // A later mount on the same point covers the earlier one.
  CHECK(mount_type(table + "27 24 0:48 / /eos/user rw - fuse.sshfs host:/ rw\n", "/eos/user/x") ==
        "fuse.sshfs");

  TempDir tmp;
  CHECK(PosixEndpoint(tmp.path()).target_warning().empty());
}
