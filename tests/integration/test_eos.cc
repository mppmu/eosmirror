// SPDX-License-Identifier: GPL-3.0-or-later
//
// Integration tests against a live EOS instance. EOSMIRROR_EOS_URL names a
// writable directory on it, e.g. root://eosmirror-mgm.eosmirror.test//eos/test,
// with the subdirectories replica2, raid6 and nochecksum as the test instance
// creates them.
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <chrono>
#include <cstdlib>

#include "doctest/doctest.h"
#include "eosmirror/engine.hh"
#include "eosmirror/eos_endpoint.hh"
#include "eosmirror/posix_endpoint.hh"
#include "eosmirror/selftest.hh"
#include "temp_dir.hh"

using namespace eosmirror;

namespace {

std::span<const std::byte> bytes(std::string_view s) {
  return {reinterpret_cast<const std::byte*>(s.data()), s.size()};
}

std::string pattern(size_t n) {
  std::string s(n, '\0');
  for (size_t i = 0; i < n; ++i) s[i] = static_cast<char>('a' + (i * 7) % 26);
  return s;
}

const char* base_url() {
  const char* url = std::getenv("EOSMIRROR_EOS_URL");
  return url && *url ? url : nullptr;
}

// A fresh directory under the given test directory of the instance.
struct RemoteDir {
  explicit RemoteDir(const std::string& label, const std::string& layout = "replica2") {
    auto parent = EosEndpoint::create(std::string(base_url()) + "/" + layout);
    REQUIRE(parent.ok());
    name = label + "-" + std::to_string(getpid()) + "-" +
           std::to_string(std::chrono::steady_clock::now().time_since_epoch().count() % 100000);
    REQUIRE(parent.value()->mkdir(name, 0755).ok());
    url = std::string(base_url()) + "/" + layout + "/" + name;
    auto ep = EosEndpoint::create(url);
    REQUIRE(ep.ok());
    endpoint = std::move(ep).value();
  }

  std::string name;
  std::string url;
  std::unique_ptr<EosEndpoint> endpoint;
};

SyncOptions test_options() {
  SyncOptions o;
  o.checkers = 2;
  o.transfers = 3;
  o.buffer_size = 64 * 1024;
  o.retry.attempts = 2;
  o.retry.initial_delay = std::chrono::milliseconds(10);
  return o;
}

// The entry of a stat that must succeed.
Entry must(Result<Entry> r) {
  REQUIRE(r.ok());
  return r.value();
}

Checksum adler(std::string_view data) {
  Hasher h(ChecksumType::Adler32);
  h.update(bytes(data));
  return h.finish();
}

}  // namespace

TEST_CASE("eos endpoint: identity, listing, symlinks, metadata and atomic writes") {
  if (!base_url()) return;
  CHECK(EosEndpoint::is_eos(base_url()));
  RemoteDir dir("endpoint");
  EosEndpoint& ep = *dir.endpoint;
  Capabilities caps = ep.capabilities();
  CHECK(caps.can_set_owner);
  CHECK(caps.can_set_mtime);
  CHECK(caps.has_symlinks);
  CHECK_FALSE(caps.symlink_owner);
  CHECK(caps.mtime_resolution == 1);
  CHECK(caps.checksum == ChecksumType::Adler32);

  auto root = ep.stat("");
  REQUIRE(root.ok());
  CHECK(root.value().type == EntryType::Directory);
  CHECK(ep.list("").value().empty());
  CHECK(ep.stat("missing").error().kind == ErrorKind::NotFound);

  REQUIRE(ep.mkdir("d", 0750).ok());
  CHECK(ep.mkdir("d", 0750).error().kind == ErrorKind::Exists);

  // An atomic upload with mtime, owner and mode, verified against the
  // checksum EOS computed.
  std::string content = pattern(200000);
  CommitSpec spec;
  spec.size = content.size();
  spec.metadata.mode = 0640;
  spec.metadata.uid = 1234;
  spec.metadata.gid = 5678;
  spec.metadata.mtime = {1600000000, 123456789};
  spec.checksum = adler(content);
  auto writer = ep.open_write("d/f", spec);
  REQUIRE(writer.ok());
  for (size_t off = 0; off < content.size(); off += 70000) {
    size_t n = std::min<size_t>(70000, content.size() - off);
    REQUIRE(writer.value()->write(off, bytes(std::string_view(content).substr(off, n))).ok());
  }
  auto committed = writer.value()->commit(spec);
  REQUIRE(committed.ok());
  CHECK(committed.value().verified);

  auto listed = ep.list("d");
  REQUIRE(listed.ok());
  REQUIRE(listed.value().size() == 1);
  const Entry& f = listed.value()[0];
  CHECK(f.name == "f");
  CHECK(f.type == EntryType::File);
  CHECK(f.size == content.size());
  CHECK(f.mode == 0640);
  CHECK(f.uid == 1234);
  CHECK(f.gid == 5678);
  CHECK(f.mtime == Timespec{1600000000, 123456789});
  auto st = ep.stat("d/f");
  REQUIRE(st.ok());
  CHECK(st.value().mtime == f.mtime);
  CHECK(st.value().uid == 1234);
  CHECK(st.value().mode == 0640);

  // A wrong checksum is rejected; EOS keeps the previous content.
  CommitSpec bad = spec;
  bad.checksum.hex = "00000000";
  auto w2 = ep.open_write("d/f", bad);
  REQUIRE(w2.ok());
  REQUIRE(w2.value()->write(0, bytes(content)).ok());
  auto s = w2.value()->commit(bad);
  REQUIRE_FALSE(s.ok());
  CHECK(s.error().kind == ErrorKind::Checksum);

  // Replacing an existing file.
  std::string other = "replacement";
  CommitSpec spec2 = spec;
  spec2.size = other.size();
  spec2.checksum = adler(other);
  spec2.metadata.mtime = {1600000001, 7};
  auto w3 = ep.open_write("d/f", spec2);
  REQUIRE(w3.ok());
  REQUIRE(w3.value()->write(0, bytes(other)).ok());
  REQUIRE(w3.value()->commit(spec2).ok());
  st = ep.stat("d/f");
  REQUIRE(st.ok());
  CHECK(st.value().size == other.size());
  CHECK(st.value().mtime == Timespec{1600000001, 7});
  CHECK(ep.list("d").value().size() == 1);  // no leftovers

  // Reading back.
  auto reader = ep.open_read("d/f");
  REQUIRE(reader.ok());
  std::string back(other.size(), '\0');
  auto n = reader.value()->read(0, {reinterpret_cast<std::byte*>(back.data()), back.size()});
  REQUIRE(n.ok());
  CHECK(back == other);

  // Symlinks: created, listed with target, replaced, mtime settable.
  REQUIRE(ep.symlink("d/l", "f").ok());
  auto l = ep.stat("d/l");
  REQUIRE(l.ok());
  CHECK(l.value().type == EntryType::Symlink);
  CHECK(l.value().link_target == "f");
  REQUIRE(ep.symlink("d/l", "../other target with spaces").ok());
  CHECK(ep.stat("d/l").value().link_target == "../other target with spaces");
  Entry lmd;
  lmd.type = EntryType::Symlink;
  lmd.mtime = {1500000000, 42};
  REQUIRE(ep.set_metadata("d/l", lmd, MetaFields::Mtime | MetaFields::Owner).ok());
  listed = ep.list("d");
  REQUIRE(listed.ok());
  REQUIRE(listed.value().size() == 2);
  for (const Entry& e : listed.value()) {
    if (e.name == "l") {
      CHECK(e.type == EntryType::Symlink);
      CHECK(e.link_target == "../other target with spaces");
      CHECK(e.mtime == Timespec{1500000000, 42});
    }
  }

  // Directory metadata.
  Entry dmd;
  dmd.type = EntryType::Directory;
  dmd.mode = 02755;
  dmd.uid = 4321;
  dmd.gid = 8765;
  dmd.mtime = {1400000000, 9};
  REQUIRE(ep.set_metadata("d", dmd, MetaFields::All).ok());
  auto d = ep.stat("d");
  REQUIRE(d.ok());
  CHECK(d.value().uid == 4321);
  CHECK(d.value().gid == 8765);
  CHECK(d.value().mode == 02755);
  CHECK(d.value().mtime == Timespec{1400000000, 9});

  CHECK(ep.remove("d", EntryType::Directory).error().kind != ErrorKind::NotFound);
  REQUIRE(ep.remove("d/l", EntryType::Symlink).ok());
  REQUIRE(ep.remove("d/f", EntryType::File).ok());
  REQUIRE(ep.remove("d", EntryType::Directory).ok());
  CHECK(ep.list("").value().empty());
}

TEST_CASE("eos endpoint: files in a directory without checksums") {
  if (!base_url()) return;
  RemoteDir dir("nochecksum", "nochecksum");
  EosEndpoint& ep = *dir.endpoint;
  CHECK(ep.directory_checksum(ep.absolute("")).value() == ChecksumType::None);
  std::string content = "no checksum here";
  CommitSpec spec;
  spec.size = content.size();
  spec.checksum = adler(content);
  spec.fields = MetaFields::Mtime;
  spec.metadata.mtime = {1600000000, 0};
  auto w = ep.open_write("f", spec);
  REQUIRE(w.ok());
  REQUIRE(w.value()->write(0, bytes(content)).ok());
  auto committed = w.value()->commit(spec);
  REQUIRE(committed.ok());
  CHECK_FALSE(committed.value().verified);
  CHECK(ep.stat("f").value().size == content.size());

  // With verification required, such a directory is refused.
  spec.require_verification = true;
  auto w2 = ep.open_write("g", spec);
  REQUIRE(w2.ok());
  REQUIRE(w2.value()->write(0, bytes(content)).ok());
  auto refused = w2.value()->commit(spec);
  REQUIRE_FALSE(refused.ok());
  CHECK(refused.error().kind == ErrorKind::Unsupported);
  CHECK(ep.stat("g").error().kind == ErrorKind::NotFound);
}

TEST_CASE("eos endpoint: names that need encoding") {
  if (!base_url()) return;
  RemoteDir dir("names");
  EosEndpoint& ep = *dir.endpoint;
  // Unencoded, these would cut the path short and add opaque parameters.
  for (std::string name : {"a b", "q?eos.atomic=0&x=1", "100%25 #1", "Gr\xc3\xb6\xc3\x9f" "e"}) {
    INFO("name ", name);
    REQUIRE(ep.mkdir(name, 0755).ok());
    std::string file = name + "/" + name;
    std::string content = "content of " + name;
    CommitSpec spec;
    spec.size = content.size();
    spec.fields = MetaFields::Mode | MetaFields::Mtime;
    spec.metadata.mode = 0640;
    spec.metadata.mtime = {1600000000, 5};
    spec.checksum = adler(content);
    auto writer = ep.open_write(file, spec);
    REQUIRE(writer.ok());
    REQUIRE(writer.value()->write(0, bytes(content)).ok());
    auto committed = writer.value()->commit(spec);
    REQUIRE(committed.ok());
    CHECK(committed.value().verified);

    Entry st = must(ep.stat(file));
    CHECK(st.size == content.size());
    CHECK(st.mode == 0640);
    CHECK(st.mtime == spec.metadata.mtime);
    auto listed = ep.list(name);
    REQUIRE(listed.ok());
    REQUIRE(listed.value().size() == 1);
    CHECK(listed.value()[0].name == name);
    auto reader = ep.open_read(file);
    REQUIRE(reader.ok());
    std::string back(content.size(), '\0');
    auto n = reader.value()->read(0, {reinterpret_cast<std::byte*>(back.data()), back.size()});
    REQUIRE(n.ok());
    CHECK(back == content);

    Entry dmd;
    dmd.type = EntryType::Directory;
    dmd.mode = 0750;
    dmd.mtime = {1500000000, 7};
    REQUIRE(ep.set_metadata(name, dmd, MetaFields::Mode | MetaFields::Mtime).ok());
    Entry d = must(ep.stat(name));
    CHECK(d.mode == 0750);
    CHECK(d.mtime == dmd.mtime);
    REQUIRE(ep.remove(file, EntryType::File).ok());
    REQUIRE(ep.remove(name, EntryType::Directory).ok());
  }

  // The symlink command takes path and target unencoded: spaces work, the
  // characters of the opaque syntax are refused.
  REQUIRE(ep.symlink("link with spaces", "target with spaces").ok());
  CHECK(must(ep.stat("link with spaces")).link_target == "target with spaces");
  REQUIRE(ep.remove("link with spaces", EntryType::Symlink).ok());
  for (auto [link, target] : {std::pair{"l?x", "t"}, std::pair{"l", "t&mgm.file.target=/x"},
                              std::pair{"l", "100%"}, std::pair{"l#", "t"}}) {
    auto refused = ep.symlink(link, target);
    REQUIRE_FALSE(refused.ok());
    CHECK(refused.error().kind == ErrorKind::Unsupported);
  }
  CHECK(ep.list("").value().empty());
}

TEST_CASE("FS to EOS and back, replica and erasure coded layouts") {
  if (!base_url()) return;
  for (const char* layout : {"replica2", "raid6"}) {
    INFO("layout ", layout);
    TempDir tmp;
    tmp.write_file("src/a.txt", "hello", 0640);
    tmp.write_file("src/big.bin", pattern(3 * 1024 * 1024 + 17), 0600);
    tmp.write_file("src/empty", "", 0444);
    tmp.write_file("src/sub/nested/file", pattern(1000), 0664);
    chmod(tmp.sub("src/sub").c_str(), 0750);
    REQUIRE(symlink("a.txt", tmp.sub("src/link").c_str()) == 0);
    REQUIRE(symlink("/abs/elsewhere", tmp.sub("src/sub/abslink").c_str()) == 0);
    struct timespec times[2] = {{1600000000, 5}, {1600000000, 5}};
    for (const char* p : {"src/a.txt", "src/big.bin", "src/sub/nested/file", "src/sub", "src/link"})
      REQUIRE(utimensat(AT_FDCWD, tmp.sub(p).c_str(), times, AT_SYMLINK_NOFOLLOW) == 0);
    if (is_root()) {
      REQUIRE(lchown(tmp.sub("src/a.txt").c_str(), 1111, 2222) == 0);
      REQUIRE(lchown(tmp.sub("src/sub").c_str(), 3333, 4444) == 0);
    }

    PosixEndpoint src(tmp.sub("src"));
    RemoteDir dir("sync", layout);
    EosEndpoint& dst = *dir.endpoint;
    SyncOptions options = test_options();
    options.preserve_owner = is_root();

    Cancellation cancel;
    Report r;
    Engine engine(src, dst, options, r, nullptr, cancel);
    REQUIRE(engine.run().ok());
    CHECK(r.stats.files_copied == 4);
    CHECK(r.stats.symlinks_created == 2);
    CHECK(r.stats.dirs_created == 2);
    CHECK(r.stats.failures == 0);

    auto a = dst.stat("a.txt").value();
    CHECK(a.mode == 0640);
    CHECK(a.mtime == Timespec{1600000000, 5});
    if (is_root()) {
      CHECK(a.uid == 1111);
      CHECK(a.gid == 2222);
      auto sub = dst.stat("sub").value();
      CHECK(sub.uid == 3333);
      CHECK(sub.mode == 0750);
    }
    CHECK(dst.stat("sub").value().mtime == Timespec{1600000000, 5});
    CHECK(dst.stat("link").value().link_target == "a.txt");
    CHECK(dst.stat("sub/abslink").value().link_target == "/abs/elsewhere");

    // A rerun changes nothing.
    Report r2;
    Engine again(src, dst, options, r2, nullptr, cancel);
    REQUIRE(again.run().ok());
    CHECK(r2.stats.files_copied == 0);
    CHECK(r2.stats.files_unchanged == 4);
    CHECK(r2.stats.symlinks_created == 0);
    CHECK(r2.stats.metadata_fixed == 0);
    CHECK(r2.stats.failures == 0);

    // Changes propagate: content, a mode, a symlink target, an extra entry.
    tmp.write_file("src/a.txt", "hello world", 0640);
    chmod(tmp.sub("src/empty").c_str(), 0600);
    unlink(tmp.sub("src/link").c_str());
    REQUIRE(symlink("empty", tmp.sub("src/link").c_str()) == 0);
    REQUIRE(dst.mkdir("extra", 0755).ok());
    SyncOptions del = options;
    del.delete_extra = true;
    Report r3;
    Engine changed(src, dst, del, r3, nullptr, cancel);
    REQUIRE(changed.run().ok());
    CHECK(r3.stats.files_copied == 1);
    CHECK(r3.stats.metadata_fixed >= 1);
    CHECK(r3.stats.symlinks_created == 1);
    CHECK(r3.stats.deleted == 1);
    CHECK(r3.stats.failures == 0);
    CHECK(dst.stat("a.txt").value().size == 11);
    CHECK(dst.stat("empty").value().mode == 0600);
    CHECK(dst.stat("link").value().link_target == "empty");

    // Back to a local directory, with everything intact.
    PosixEndpoint back(tmp.sub("back"));
    Report r4;
    Engine restore(dst, back, options, r4, nullptr, cancel);
    REQUIRE(restore.run().ok());
    CHECK(r4.stats.files_copied == 4);
    CHECK(r4.stats.symlinks_created == 2);
    CHECK(r4.stats.failures == 0);
    CHECK(tmp.read_file("back/a.txt") == "hello world");
    CHECK(tmp.read_file("back/big.bin") == tmp.read_file("src/big.bin"));
    CHECK(back.stat("sub").value().mtime == Timespec{1600000000, 5});
    CHECK(back.stat("link").value().link_target == "empty");
    if (is_root()) CHECK(back.stat("a.txt").value().uid == 1111);

    Report r5;
    Engine verify(src, back, options, r5, nullptr, cancel);
    REQUIRE(verify.run().ok());
    CHECK(r5.stats.files_copied == 0);
    CHECK(r5.stats.metadata_fixed == 0);
  }
}

TEST_CASE("EOS to EOS between directories") {
  if (!base_url()) return;
  RemoteDir a("eos2eos-src", "replica2");
  RemoteDir b("eos2eos-dst", "raid6");
  std::string content = pattern(2 * 1024 * 1024 + 3);
  CommitSpec spec;
  spec.size = content.size();
  spec.checksum = adler(content);
  spec.metadata.mode = 0644;
  spec.metadata.mtime = {1600000000, 1};
  auto w = a.endpoint->open_write("f", spec);
  REQUIRE(w.ok());
  REQUIRE(w.value()->write(0, bytes(content)).ok());
  REQUIRE(w.value()->commit(spec).ok());
  REQUIRE(a.endpoint->mkdir("d", 0755).ok());
  REQUIRE(a.endpoint->symlink("d/l", "../f").ok());

  Cancellation cancel;
  Report r;
  SyncOptions options = test_options();
  options.preserve_owner = false;
  Engine engine(*a.endpoint, *b.endpoint, options, r, nullptr, cancel);
  REQUIRE(engine.run().ok());
  CHECK(r.stats.files_copied == 1);
  CHECK(r.stats.symlinks_created == 1);
  CHECK(r.stats.failures == 0);
  CHECK(must(b.endpoint->stat("f")).size == content.size());
  CHECK(must(b.endpoint->stat("f")).mtime == Timespec{1600000000, 1});
  CHECK((b.endpoint->query_checksum(b.endpoint->absolute("f")).value() == spec.checksum));
  CHECK(must(b.endpoint->stat("d/l")).link_target == "../f");
}

TEST_CASE("selftest against EOS") {
  if (!base_url()) return;
  for (const char* layout : {"replica2", "raid6", "nochecksum"}) {
    INFO("layout ", layout);
    RemoteDir dir("selftest", layout);
    SelftestReport r = run_selftest(*dir.endpoint, true);
    for (const SelftestCheck& c : r.checks) {
      INFO(c.name, ": ", c.message);
      CHECK(c.ok);
    }
    CHECK(r.ok());
    CHECK(dir.endpoint->list("").value().empty());
  }
}
