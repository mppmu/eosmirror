// SPDX-License-Identifier: GPL-3.0-or-later
#include "eosmirror/posix_endpoint.hh"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <filesystem>

#include "doctest/doctest.h"
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
  const auto& entries = listed.value();
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
  REQUIRE(during.value().size() == 1);
  CHECK(ep.is_temporary(during.value()[0].name));
  CHECK_FALSE(ep.is_temporary("out.bin"));

  REQUIRE(writer.value()->write(6, bytes("world")).ok());
  hasher.update(bytes("world"));
  spec.checksum = hasher.finish();
  REQUIRE(writer.value()->commit(spec).ok());

  CHECK(tmp.read_file("out.bin") == "hello world");
  auto st = ep.stat("out.bin");
  REQUIRE(st.ok());
  CHECK(st.value().mode == 0600);
  CHECK(st.value().mtime == Timespec{1600000000, 42});
  CHECK(ep.list("").value().size() == 1);  // the temporary is gone

  SUBCASE("a size mismatch is rejected and leaves no trace") {
    auto w = ep.open_write("short.bin", spec);
    REQUIRE(w.ok());
    REQUIRE(w.value()->write(0, bytes("abc")).ok());
    Status s = w.value()->commit(spec);
    REQUIRE_FALSE(s.ok());
    CHECK(s.error().kind == ErrorKind::Changed);
    CHECK_FALSE(fs::exists(tmp.sub("short.bin")));
    CHECK(ep.list("").value().size() == 1);
  }

  SUBCASE("non-sequential writes are rejected") {
    auto w = ep.open_write("gap.bin", spec);
    REQUIRE(w.ok());
    CHECK_FALSE(w.value()->write(5, bytes("x")).ok());
    w.value()->abort();
    CHECK(ep.list("").value().size() == 1);
  }

  SUBCASE("a dropped writer cleans up") {
    {
      auto w = ep.open_write("dropped.bin", spec);
      REQUIRE(w.ok());
      REQUIRE(w.value()->write(0, bytes("x")).ok());
    }
    CHECK(ep.list("").value().size() == 1);
  }

  SUBCASE("commit replaces an existing file") {
    auto w = ep.open_write("out.bin", spec);
    REQUIRE(w.ok());
    REQUIRE(w.value()->write(0, bytes("HELLO WORLD")).ok());
    REQUIRE(w.value()->commit(spec).ok());
    CHECK(tmp.read_file("out.bin") == "HELLO WORLD");
  }
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
  Status s = w.value()->commit(spec);
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
  CHECK(ep.list("d").value().size() == 1);

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
