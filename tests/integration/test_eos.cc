// SPDX-License-Identifier: GPL-3.0-or-later
//
// Integration tests against a live EOS instance. EOSMIRROR_EOS_URL names a
// writable directory on it, e.g. root://eosmirror-mgm.eosmirror.test//eos/test,
// with the subdirectories replica2, raid6 and nochecksum as the test instance
// creates them. The tests run as root, except for the one of a sudoer's
// identity, which as-sudoer.sh runs on its own.
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <XrdCl/XrdClFile.hh>
#include <XrdCl/XrdClURL.hh>
#include <XrdCl/XrdClXRootDResponses.hh>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <map>
#include <utility>
#include <vector>

#include "doctest/doctest.h"
#include "eosmirror/copy.hh"
#include "eosmirror/endpoints.hh"
#include "eosmirror/engine.hh"
#include "eosmirror/eos_endpoint.hh"
#include "eosmirror/posix_endpoint.hh"
#include "eosmirror/selftest.hh"
#include "request_log.hh"
#include "stderr_capture.hh"
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
    // Unique on an instance that outlives many test runs.
    name = label + "-" + std::to_string(getpid()) + "-" + random_suffix();
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
  o.min_transfers = o.max_transfers = 3;
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

// Writes a file through an atomic upload.
void put(EosEndpoint& ep, const RelPath& path, const std::string& content,
         Timespec mtime = {1600000000, 0}) {
  CommitSpec spec;
  spec.size = content.size();
  spec.fields = MetaFields::Mtime;
  spec.metadata.mtime = mtime;
  spec.checksum = adler(content);
  auto w = ep.open_write(path, spec);
  REQUIRE(w.ok());
  REQUIRE(w.value()->write(0, bytes(content)).ok());
  REQUIRE(w.value()->commit(spec).ok());
}

std::string get(Endpoint& ep, const RelPath& path) {
  auto st = ep.stat(path);
  REQUIRE(st.ok());
  auto reader = ep.open_read(path);
  REQUIRE(reader.ok());
  std::string data(st.value().size, '\0');
  auto n = reader.value()->read(0, {reinterpret_cast<std::byte*>(data.data()), data.size()});
  REQUIRE(n.ok());
  data.resize(n.value());
  return data;
}

const Entry* find(const std::vector<Entry>& entries, std::string_view name) {
  auto it = std::find_if(entries.begin(), entries.end(), [&](const Entry& e) { return e.name == name; });
  return it == entries.end() ? nullptr : &*it;
}

// The test directory as seen by nobody: the instance maps root that asks
// for another identity (eos.ruid) to nobody, who gets find limits there.
std::unique_ptr<EosEndpoint> as_nobody(const std::string& url) {
  auto ep = EosEndpoint::create(url + "?eos.ruid=65534&eos.rgid=65534");
  REQUIRE(ep.ok());
  auto who = ep.value()->proc("mgm.cmd=whoami");
  REQUIRE(who.ok());
  INFO("whoami: ", who.value().out);
  REQUIRE(who.value().out.find("uid=65534 ") != std::string::npos);
  REQUIRE(who.value().out.find("sudo*") == std::string::npos);
  return std::move(ep).value();
}

}  // namespace

TEST_CASE("eos endpoint: identity, listing, symlinks, metadata and atomic writes") {
  if (!base_url()) return;
  CHECK(EosEndpoint::is_eos(base_url()).value());
  RemoteDir dir("endpoint");
  EosEndpoint& ep = *dir.endpoint;
  Capabilities caps = ep.capabilities();
  CHECK(caps.has_owners);
  CHECK(caps.can_set_owner);
  CHECK(caps.can_set_mtime);
  CHECK(caps.has_symlinks);
  CHECK(caps.symlink_owner);
  CHECK(caps.mtime_resolution == 1);
  CHECK(caps.file_mode_bits == 0777);
  CHECK(caps.dir_mode_bits == 03777);
  CHECK(caps.checksum == ChecksumType::Adler32);

  auto root = ep.stat("");
  REQUIRE(root.ok());
  CHECK(root.value().type == EntryType::Directory);
  CHECK(ep.list("").value().entries.empty());
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
  REQUIRE(listed.value().entries.size() == 1);
  const Entry& f = listed.value().entries[0];
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

  // A checksum mismatch found after the upload is in place removes it, so
  // that its size and mtime cannot pass for a good copy.
  std::string corrupt = pattern(1000);
  CommitSpec bad = spec;
  bad.size = corrupt.size();
  bad.checksum.hex = "00000000";
  auto w2 = ep.open_write("d/f", bad);
  REQUIRE(w2.ok());
  REQUIRE(w2.value()->write(0, bytes(corrupt)).ok());
  auto s = w2.value()->commit(bad);
  REQUIRE_FALSE(s.ok());
  CHECK(s.error().kind == ErrorKind::Checksum);
  CHECK(ep.stat("d/f").error().kind == ErrorKind::NotFound);

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
  CHECK(ep.list("d").value().entries.size() == 1);  // no leftovers

  // Reading back.
  auto reader = ep.open_read("d/f");
  REQUIRE(reader.ok());
  std::string back(other.size(), '\0');
  auto n = reader.value()->read(0, {reinterpret_cast<std::byte*>(back.data()), back.size()});
  REQUIRE(n.ok());
  CHECK(back == other);

  // Symlinks: created, listed with target, replaced, owner and mtime settable.
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
  lmd.uid = 2345;
  lmd.gid = 3456;
  REQUIRE(ep.set_metadata("d/l", lmd, MetaFields::Mtime | MetaFields::Owner).ok());
  listed = ep.list("d");
  REQUIRE(listed.ok());
  REQUIRE(listed.value().entries.size() == 2);
  for (const Entry& e : listed.value().entries) {
    if (e.name == "l") {
      CHECK(e.type == EntryType::Symlink);
      CHECK(e.link_target == "../other target with spaces");
      CHECK(e.mtime == Timespec{1500000000, 42});
      CHECK(e.uid == 2345);
      CHECK(e.gid == 3456);
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
  CHECK(ep.list("").value().entries.empty());
}

TEST_CASE("eos endpoint: files in a directory without checksums") {
  if (!base_url()) return;
  RemoteDir dir("nochecksum", "nochecksum");
  EosEndpoint& ep = *dir.endpoint;
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
  CHECK(ep.query_checksum(ep.absolute("f")).value().type == ChecksumType::None);

  // With verification required, the upload is refused and taken away again.
  spec.require_verification = true;
  auto w2 = ep.open_write("f", spec);
  REQUIRE(w2.ok());
  REQUIRE(w2.value()->write(0, bytes(content)).ok());
  auto refused = w2.value()->commit(spec);
  REQUIRE_FALSE(refused.ok());
  CHECK(refused.error().kind == ErrorKind::Unsupported);
  CHECK(ep.stat("f").error().kind == ErrorKind::NotFound);

  // A directory whose files get another checksum type: not compared either.
  REQUIRE(ep.mkdir("crc", 0755).ok());
  auto attr = ep.proc("mgm.cmd=attr&mgm.subcmd=set&mgm.attr.key=sys.forced.checksum"
                      "&mgm.attr.value=crc32c&mgm.path=" + eos_encoded_path(ep.absolute("crc")) +
                      "&eos.encodepath=1");
  REQUIRE(attr.ok());
  REQUIRE(attr.value().retc == 0);
  spec.require_verification = false;
  auto w3 = ep.open_write("crc/f", spec);
  REQUIRE(w3.ok());
  REQUIRE(w3.value()->write(0, bytes(content)).ok());
  auto other = w3.value()->commit(spec);
  REQUIRE(other.ok());
  CHECK_FALSE(other.value().verified);
  CHECK(ep.query_checksum(ep.absolute("crc/f")).value().type == ChecksumType::None);
  CHECK(must(ep.stat("crc/f")).size == content.size());
}

TEST_CASE("eos endpoint: aborted uploads leave the previous file as it was") {
  if (!base_url()) return;
  for (const char* layout : {"replica2", "raid6"}) {
    INFO("layout ", layout);
    RemoteDir dir("abort", layout);
    EosEndpoint& ep = *dir.endpoint;
    std::string old = "the previous content";
    put(ep, "f", old, {1500000000, 1});

    std::string content = pattern(3 * 1024 * 1024);
    CommitSpec spec;
    spec.size = content.size();
    spec.fields = MetaFields::Mtime;
    spec.metadata.mtime = {1600000000, 2};
    spec.checksum = adler(content);
    auto w = ep.open_write("f", spec);
    REQUIRE(w.ok());
    REQUIRE(w.value()->write(0, bytes(std::string_view(content).substr(0, 1024 * 1024))).ok());
    w.value()->abort();
    CHECK(get(ep, "f") == old);
    CHECK(must(ep.stat("f")).mtime == Timespec{1500000000, 1});

    // A writer dropped without commit is aborted, and so is a commit with
    // fewer bytes than announced.
    auto w2 = ep.open_write("f", spec);
    REQUIRE(w2.ok());
    REQUIRE(w2.value()->write(0, bytes(std::string_view(content).substr(0, 1000))).ok());
    w2.value().reset();
    auto w3 = ep.open_write("f", spec);
    REQUIRE(w3.ok());
    REQUIRE(w3.value()->write(0, bytes(std::string_view(content).substr(0, 1000))).ok());
    auto short_commit = w3.value()->commit(spec);
    REQUIRE_FALSE(short_commit.ok());
    CHECK(short_commit.error().kind == ErrorKind::Changed);
    CHECK(get(ep, "f") == old);

    // An abort with a full window of writes in flight.
    auto w4 = ep.open_write("f", spec);
    REQUIRE(w4.ok());
    for (size_t off = 0; off < 2 * 1024 * 1024; off += 512 * 1024)
      REQUIRE(
          w4.value()->write(off, bytes(std::string_view(content).substr(off, 512 * 1024))).ok());
    w4.value()->abort();
    CHECK(get(ep, "f") == old);
    CHECK(must(ep.stat("f")).mtime == Timespec{1500000000, 1});

    // A copy that is cancelled halfway.
    TempDir tmp;
    tmp.write_file("f", content);
    PosixEndpoint src(tmp.path());
    Cancellation cancel;
    CopyOptions options;
    options.preserve_owner = false;
    options.on_chunk = [&](uint64_t) { cancel.request(); };
    BufferPool pool(256 * 1024);
    auto copied = copy_file(src, ep, "f", must(src.stat("f")), options, pool, cancel);
    REQUIRE_FALSE(copied.ok());
    CHECK(copied.error().kind == ErrorKind::Cancelled);
    CHECK(get(ep, "f") == old);
    CHECK(ep.list("").value().entries.size() == 1);
  }
}

TEST_CASE("FS to EOS: --delete leaves another writer's upload in progress alone") {
  if (!base_url()) return;
  RemoteDir dir("inflight");
  EosEndpoint& ep = *dir.endpoint;
  // An atomic upload with the old mtime of its source, partly written and
  // held open.
  std::string server = parse_endpoint_url(dir.url).value().server;
  XrdCl::File upload;
  REQUIRE(upload
              .Open(server + "/" + eos_request_path(ep.absolute("big")) +
                        "&eos.atomic=1&eos.mtime=1500000000.000000000",
                    XrdCl::OpenFlags::Delete | XrdCl::OpenFlags::Write,
                    XrdCl::Access::UR | XrdCl::Access::UW)
              .IsOK());
  std::string part = pattern(100000);
  REQUIRE(upload.Write(0, static_cast<uint32_t>(part.size()), part.data()).IsOK());

  // EOS lists it under a hidden name with that mtime, and the time it was
  // created as its ctime.
  int64_t now = std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::system_clock::now().time_since_epoch())
                    .count();
  auto listed = ep.list("");
  REQUIRE(listed.ok());
  REQUIRE(listed.value().entries.size() == 1);
  const Entry& hidden = listed.value().entries[0];
  CHECK(ep.is_temporary(hidden));
  CHECK(hidden.mtime.sec == 1500000000);
  CHECK(hidden.ctime.sec > now - 600);

  TempDir tmp;
  tmp.write_file("src/f", "x");
  PosixEndpoint src(tmp.sub("src"));
  SyncOptions options = test_options();
  options.delete_extra = true;
  Cancellation cancel;
  Report r;
  Engine engine(src, ep, options, r, nullptr, cancel);
  REQUIRE(engine.run().ok());
  CHECK(r.stats.failures == 0);
  CHECK(r.stats.deleted == 0);
  CHECK(r.stats.files_copied == 1);

  // The upload completes as if nothing had happened.
  REQUIRE(upload.Close().IsOK());
  CHECK(get(ep, "big") == part);
  CHECK(must(ep.stat("big")).mtime == Timespec{1500000000, 0});
}

TEST_CASE("eos endpoint: pipelined uploads and downloads of many chunks") {
  if (!base_url()) return;
  std::string content = pattern(5 * 1024 * 1024 + 17);
  TempDir tmp;
  tmp.write_file("src/f", content);
  PosixEndpoint src(tmp.sub("src"));
  for (const char* layout : {"raid6", "replica2"}) {
    for (int window : {4, 1}) {
      INFO("layout ", layout, ", window ", window);
      RemoteDir dir("pipelined", layout);
      XrdOptions xo;
      xo.write_window = window;
      xo.read_window = window;
      auto ep = EosEndpoint::create(dir.url, xo);
      REQUIRE(ep.ok());
      Cancellation cancel;
      CopyOptions options;
      options.preserve_owner = false;
      BufferPool pool(64 * 1024);  // 81 chunks
      auto up = copy_file(src, *ep.value(), "f", must(src.stat("f")), options, pool, cancel);
      REQUIRE(up.ok());
      CHECK(up.value().verified);
      CHECK(up.value().bytes == content.size());
      CHECK(get(*ep.value(), "f") == content);
      CHECK((ep.value()->query_checksum(ep.value()->absolute("f")).value() == adler(content)));

      // And back, verified against the checksum the listing gives.
      auto listed = ep.value()->list("");
      REQUIRE(listed.ok());
      REQUIRE(find(listed.value().entries, "f"));
      CHECK((find(listed.value().entries, "f")->checksum == adler(content)));
      std::string back_dir = std::string("back-") + layout + std::to_string(window);
      PosixEndpoint back(tmp.sub(back_dir));
      REQUIRE(back.mkdir("", 0755).ok());
      const Entry& entry = *find(listed.value().entries, "f");
      auto down = copy_file(*ep.value(), back, "f", entry, options, pool, cancel);
      REQUIRE(down.ok());
      CHECK(down.value().verified);
      CHECK(tmp.read_file(back_dir + "/f") == content);
      CHECK(pool.available() == pool.allocated());
      CHECK(pool.allocated() <= static_cast<size_t>(window) + 2);
    }
  }
}

TEST_CASE("eos endpoint: an upload whose file disappears fails once and leaves nothing") {
  if (!base_url()) return;
  RemoteDir dir("vanish");
  EosEndpoint& ep = *dir.endpoint;
  std::string content = pattern(4 * 1024 * 1024);
  TempDir tmp;
  tmp.write_file("f", content);
  PosixEndpoint src(tmp.path());
  CopyOptions options;
  options.preserve_owner = false;
  int chunks = 0;
  bool removed = false;
  options.on_chunk = [&](uint64_t) {
    if (++chunks != 3) return;
    // The atomic upload's hidden file goes behind the writer's back.
    auto listed = ep.list("");
    REQUIRE(listed.ok());
    for (const Entry& e : listed.value().entries)
      if (e.name.starts_with(".sys.a#.")) removed = ep.remove(e.name, EntryType::File).ok();
  };
  BufferPool pool(256 * 1024);
  Cancellation cancel;
  StderrCapture err;
  auto copied = copy_file(src, ep, "f", must(src.stat("f")), options, pool, cancel);
  CHECK(removed);
  REQUIRE_FALSE(copied.ok());
  INFO("error: ", copied.error().describe());
  CHECK(is_transient(copied.error().kind));
  CHECK(ep.stat("f").error().kind == ErrorKind::NotFound);
  CHECK(ep.list("").value().entries.empty());
  CHECK(pool.available() == pool.allocated());
}

TEST_CASE("EOS to FS: downloads are verified, and a read that fails midway leaves nothing") {
  if (!base_url()) return;
  RemoteDir dir("download");
  EosEndpoint& ep = *dir.endpoint;
  std::string big = pattern(3 * 1024 * 1024 + 5);
  put(ep, "a", big);
  put(ep, "b", "small");
  REQUIRE(ep.mkdir("crc", 0755).ok());
  auto attr = ep.proc("mgm.cmd=attr&mgm.subcmd=set&mgm.attr.key=sys.forced.checksum"
                      "&mgm.attr.value=crc32c&mgm.path=" + eos_encoded_path(ep.absolute("crc")) +
                      "&eos.encodepath=1");
  REQUIRE(attr.ok());
  REQUIRE(attr.value().retc == 0);
  put(ep, "crc/bad", big);
  // The crc32c checksum relabelled as adler32. The storage node finds the
  // mismatch itself: it fails the read that reaches the end of the file, and
  // later opens.
  auto relabel = ep.proc("mgm.cmd=file&mgm.subcmd=layout&mgm.path=" + ep.absolute("crc/bad") +
                         "&mgm.file.layout.checksum=adler");
  REQUIRE(relabel.ok());
  INFO("file layout: ", relabel.value().out, relabel.value().err);
  REQUIRE(relabel.value().retc == 0);
  auto listed = ep.list("crc");
  REQUIRE(listed.ok());
  const Entry* bad = find(listed.value().entries, "bad");
  REQUIRE(bad);
  REQUIRE(bad->checksum.type == ChecksumType::Adler32);
  REQUIRE(bad->checksum != adler(big));

  TempDir tmp;
  PosixEndpoint dst(tmp.sub("dst"));
  REQUIRE(dst.mkdir("", 0755).ok());
  REQUIRE(dst.mkdir("crc", 0755).ok());
  CopyOptions copy_options;
  copy_options.preserve_owner = false;
  int chunks = 0;
  copy_options.on_chunk = [&](uint64_t) { ++chunks; };
  BufferPool pool(64 * 1024);
  Cancellation cancel;
  auto failed = copy_file(ep, dst, "crc/bad", *bad, copy_options, pool, cancel);
  REQUIRE_FALSE(failed.ok());
  INFO("error: ", failed.error().describe());
  CHECK(is_transient(failed.error().kind));
  CHECK(chunks > 0);  // midway
  CHECK(dst.list("crc").value().entries.empty());  // neither the file nor a temporary
  CHECK(pool.available() == pool.allocated());

  // A sync verifies what it downloads, and reports the broken file once.
  SyncOptions options = test_options();
  options.preserve_owner = false;
  Report r;
  Engine engine(ep, dst, options, r, nullptr, cancel);
  REQUIRE(engine.run().ok());
  CHECK(r.stats.files_copied == 2);
  CHECK(r.stats.files_unverified == 0);
  REQUIRE(r.stats.failures == 1);
  CHECK(r.failures()[0].path == "crc/bad");
  CHECK(tmp.read_file("dst/a") == big);
  CHECK(tmp.read_file("dst/b") == "small");
  CHECK(dst.list("crc").value().entries.empty());
}

TEST_CASE("EOS to FS: files are read with the listing's metadata, without MGM stats") {
  if (!base_url()) return;
  std::string mgm = XrdCl::URL(base_url()).GetHostName();
  for (const char* layout : {"replica2", "raid6"}) {
    INFO("layout ", layout);
    constexpr int kFiles = 20;
    TempDir tmp;
    for (int i = 0; i < kFiles; ++i) {
      std::string name = "src/d/f" + std::to_string(i);
      tmp.write_file(name, pattern(1000 + static_cast<size_t>(i)));
      struct timespec times[2] = {{1600000000 + i, 123456789}, {1600000000 + i, 123456789}};
      REQUIRE(utimensat(AT_FDCWD, tmp.sub(name).c_str(), times, 0) == 0);
    }
    tmp.write_file("src/empty", "");
    PosixEndpoint src(tmp.sub("src"));
    RemoteDir dir("listed", layout);
    EosEndpoint& ep = *dir.endpoint;
    SyncOptions options = test_options();
    options.preserve_owner = false;
    Cancellation cancel;
    Report up;
    Engine upload(src, ep, options, up, nullptr, cancel);
    REQUIRE(upload.run().ok());
    REQUIRE(up.stats.files_copied == kFiles + 1);

    PosixEndpoint dst(tmp.sub("dst"));
    Report r;
    RequestLog::start();
    {
      Engine download(ep, dst, options, r, nullptr, cancel);
      REQUIRE(download.run().ok());
    }
    auto sent = RequestLog::stop();
    CHECK(r.stats.files_copied == kFiles + 1);
    CHECK(r.stats.files_unverified == 0);
    CHECK(r.stats.failures == 0);
    CHECK(must(dst.stat("d/f7")).mtime == Timespec{1600000007, 123456789});
    CHECK(tmp.read_file("dst/d/f7") == pattern(1007));
    // Per file, the MGM sees the open, which redirects to the storage node
    // (or, for the empty file of the replica layout, serves it), and the
    // storage node a stat after the read; the only stat on the MGM is the
    // one of the top directory, the other opens are the two listings.
    INFO("requests: ", sent.size());
    CHECK(RequestLog::count(sent, mgm, "kXR_query") == 1);
    CHECK(RequestLog::count(sent, mgm, "kXR_stat") == 0);
    CHECK(RequestLog::count(sent, mgm, "kXR_open") == kFiles + 1 + 2);
    CHECK(RequestLog::count(sent, mgm, "kXR_stat", false) == kFiles);
  }
}

TEST_CASE("FS to EOS: an upload is confirmed by one query of the MGM") {
  if (!base_url()) return;
  std::string mgm = XrdCl::URL(base_url()).GetHostName();
  // The requests to the MGM of runs that upload 10 and 30 files.
  std::map<int, std::vector<std::string>> sent;
  for (int files : {10, 30}) {
    TempDir tmp;
    for (int i = 0; i < files; ++i) tmp.write_file("src/d/f" + std::to_string(i), pattern(100));
    PosixEndpoint src(tmp.sub("src"));
    RemoteDir dir("confirmed");
    SyncOptions options = test_options();
    options.preserve_owner = false;
    Cancellation cancel;
    Report r;
    RequestLog::start();
    {
      Engine upload(src, *dir.endpoint, options, r, nullptr, cancel);
      REQUIRE(upload.run().ok());
    }
    sent[files] = RequestLog::stop();
    CHECK(r.stats.files_copied == files);
    CHECK(r.stats.files_unverified == 0);
  }
  // Per file, the open, which redirects to the storage node, and one query
  // that confirms the upload by its file id, size, mtime and checksum
  // (getfmd); no stats (owners are not set here, and new files get their
  // mode with the open).
  auto per_file = [&](std::string_view request) {
    return (RequestLog::count(sent[30], mgm, request) - RequestLog::count(sent[10], mgm, request)) /
           20.0;
  };
  CHECK(per_file("kXR_open") == 1);
  CHECK(per_file("kXR_query") == 1);
  CHECK(per_file("kXR_stat") == 0);
  CHECK(per_file("kXR_read") == 0);
}

namespace {

// Appends to a file on EOS through an open for update.
void append(const std::string& url, std::string_view data) {
  XrdCl::File file;
  REQUIRE(file.Open(url, XrdCl::OpenFlags::Update).IsOK());
  XrdCl::StatInfo* info = nullptr;
  REQUIRE(file.Stat(true, info).IsOK());
  uint64_t size = info->GetSize();
  delete info;
  REQUIRE(file.Write(size, static_cast<uint32_t>(data.size()), data.data()).IsOK());
  REQUIRE(file.Close().IsOK());
}

}  // namespace

TEST_CASE("EOS to FS: files that changed since the listing or while they are read") {
  if (!base_url()) return;
  RemoteDir dir("changing");
  EosEndpoint& ep = *dir.endpoint;
  TempDir tmp;
  PosixEndpoint dst(tmp.sub("dst"));
  REQUIRE(dst.mkdir("", 0755).ok());
  CopyOptions options;
  options.preserve_owner = false;
  BufferPool pool(64 * 1024);
  Cancellation cancel;
  auto listed = [&](EosEndpoint& from, const RelPath& name) {
    auto entries = from.list("");
    REQUIRE(entries.ok());
    const Entry* e = find(entries.value().entries, name);
    REQUIRE(e);
    return *e;
  };

  // Replaced with another size: copied as it is now, stated through the MGM.
  put(ep, "f", "listed content", {1600000000, 1});
  Entry old = listed(ep, "f");
  put(ep, "f", "replaced with a longer content", {1600000500, 2});
  auto copied = copy_file(ep, dst, "f", old, options, pool, cancel);
  REQUIRE(copied.ok());
  CHECK(tmp.read_file("dst/f") == "replaced with a longer content");
  CHECK(must(dst.stat("f")).mtime == Timespec{1600000500, 2});

  // Replaced once more after it was opened, with the size of the file read:
  // the MGM's stat is of another file, whose metadata are not those of the
  // data read.
  put(ep, "h", "version A", {1600000000, 1});
  Entry a = listed(ep, "h");
  put(ep, "h", "version B, longer", {1600000100, 1});
  auto reader = ep.open_read("h");
  REQUIRE(reader.ok());
  put(ep, "h", "version C, longer", {1600000200, 1});
  auto state = reader.value()->state(a);
  REQUIRE_FALSE(state.ok());
  CHECK(state.error().kind == ErrorKind::Changed);

  // Rewritten with its size: the data does not match the listed checksum,
  // and the MGM has another mtime for it.
  old = listed(ep, "f");
  put(ep, "f", "REPLACED WITH A LONGER CONTENT", {1600000600, 3});
  auto rewritten = copy_file(ep, dst, "f", old, options, pool, cancel);
  REQUIRE_FALSE(rewritten.ok());
  INFO("error: ", rewritten.error().describe());
  CHECK(rewritten.error().kind == ErrorKind::Changed);
  CHECK(tmp.read_file("dst/f") == "replaced with a longer content");

  // Appended to while it is read: changed, and copied as it is now when the
  // copy is retried with the same listed entry.
  std::string content = pattern(300000);
  put(ep, "g", content);
  old = listed(ep, "g");
  CopyOptions appending = options;
  bool appended = false;
  appending.on_chunk = [&](uint64_t) {
    if (!std::exchange(appended, true)) append(dir.url + "/g", "more");
  };
  auto grown = copy_file(ep, dst, "g", old, appending, pool, cancel);
  REQUIRE(appended);
  REQUIRE_FALSE(grown.ok());
  INFO("error: ", grown.error().describe());
  CHECK(grown.error().kind == ErrorKind::Changed);
  CHECK_FALSE(dst.stat("g").ok());
  auto retried = copy_file(ep, dst, "g", old, options, pool, cancel);
  REQUIRE(retried.ok());
  CHECK(tmp.read_file("dst/g") == content + "more");
  CHECK(must(dst.stat("g")).mtime == must(ep.stat("g")).mtime);

  // Gone: not found, which a run counts as vanished from the source.
  old = listed(ep, "g");
  REQUIRE(ep.remove("g", EntryType::File).ok());
  auto gone = copy_file(ep, dst, "g", old, options, pool, cancel);
  REQUIRE_FALSE(gone.ok());
  CHECK(gone.error().kind == ErrorKind::NotFound);
  CHECK(pool.available() == pool.allocated());

  // Without checksums in the listing, a file rewritten with its size gets
  // the listed mtime, which differs from the source's: the next run copies
  // it again.
  RemoteDir plain("changing", "nochecksum");
  put(*plain.endpoint, "f", "aaaa", {1600000000, 1});
  old = listed(*plain.endpoint, "f");
  CHECK(old.checksum.type == ChecksumType::None);
  put(*plain.endpoint, "f", "bbbb", {1600000700, 0});
  PosixEndpoint dst2(tmp.sub("dst2"));
  REQUIRE(dst2.mkdir("", 0755).ok());
  REQUIRE(copy_file(*plain.endpoint, dst2, "f", old, options, pool, cancel).ok());
  CHECK(tmp.read_file("dst2/f") == "bbbb");
  CHECK(must(dst2.stat("f")).mtime == Timespec{1600000000, 1});
  SyncOptions sync = test_options();
  sync.preserve_owner = false;
  Report r;
  Engine engine(*plain.endpoint, dst2, sync, r, nullptr, cancel);
  REQUIRE(engine.run().ok());
  CHECK(r.stats.files_copied == 1);
  CHECK(must(dst2.stat("f")).mtime == Timespec{1600000700, 0});
}

TEST_CASE("eos endpoint: roots reached through symlinks") {
  if (!base_url()) return;
  RemoteDir dir("symlinked");
  EosEndpoint& ep = *dir.endpoint;
  REQUIRE(ep.mkdir("real", 0755).ok());
  REQUIRE(ep.mkdir("real/sub", 0755).ok());
  put(ep, "real/sub/f", "below a symlink");
  REQUIRE(ep.symlink("rel", "real").ok());
  REQUIRE(ep.symlink("abs", ep.absolute("real/sub")).ok());
  REQUIRE(ep.symlink("chain", "rel/./sub/../sub").ok());
  REQUIRE(ep.symlink("loop", "loop").ok());

  for (const char* root : {"rel/sub", "abs", "chain"}) {
    INFO("root ", root);
    auto linked = EosEndpoint::create(dir.url + "/" + root);
    REQUIRE(linked.ok());
    CHECK(linked.value()->describe() == dir.url + "/" + root);
    CHECK(linked.value()->absolute("") == ep.absolute("real/sub"));
    auto listed = linked.value()->list("");
    REQUIRE(listed.ok());
    REQUIRE(listed.value().entries.size() == 1);
    CHECK(listed.value().entries[0].name == "f");
    CHECK(get(*linked.value(), "f") == "below a symlink");
  }

  // A root that does not exist yet below a symlink, as a sync creates it.
  TempDir tmp;
  tmp.write_file("src/a", "a");
  tmp.write_file("src/d/b", "b");
  PosixEndpoint src(tmp.sub("src"));
  auto target = EosEndpoint::create(dir.url + "/rel/new");
  REQUIRE(target.ok());
  SyncOptions options = test_options();
  options.preserve_owner = false;
  Cancellation cancel;
  Report r;
  Engine engine(src, *target.value(), options, r, nullptr, cancel);
  REQUIRE(engine.run().ok());
  CHECK(r.stats.files_copied == 2);
  CHECK(r.stats.failures == 0);
  CHECK(get(ep, "real/new/d/b") == "b");

  auto loop = EosEndpoint::create(dir.url + "/loop/x");
  REQUIRE_FALSE(loop.ok());
  // A root with a .. component would be resolved by the server after its
  // symlinks: refused.
  CHECK_FALSE(EosEndpoint::create(dir.url + "/missing/../rel/new").ok());

  // Trees are compared by real path: through symlinks, also for roots that
  // do not exist yet, and with the port spelled out.
  std::string url = dir.url;
  auto path = url.find('/', url.find("://") + 3);
  auto real = EosEndpoint::create(url.substr(0, path) + ":1094" + url.substr(path) + "/real");
  REQUIRE(real.ok());
  for (const char* other : {"rel", "abs", "rel/new/deeper", "chain"}) {
    INFO("other ", other);
    auto linked = EosEndpoint::create(dir.url + "/" + other);
    REQUIRE(linked.ok());
    CHECK_FALSE(check_disjoint(*real.value(), *linked.value()).ok());
    CHECK_FALSE(check_disjoint(*linked.value(), *real.value()).ok());
  }
  auto beside = EosEndpoint::create(dir.url + "/really");
  REQUIRE(beside.ok());
  CHECK(check_disjoint(*real.value(), *beside.value()).ok());
}

TEST_CASE("eos endpoint: listings for an identity with find limits") {
  if (!base_url()) return;
  RemoteDir dir("limits");
  EosEndpoint& ep = *dir.endpoint;
  // The instance limits nobody to 20 files and 10 directories per find,
  // counting the files of subdirectories: "many" is cut right after the
  // directory itself, "wide" among its subdirectories.
  REQUIRE(ep.mkdir("many", 0755).ok());
  for (int i = 0; i < 25; ++i) put(ep, "many/f" + std::to_string(i), "file " + std::to_string(i));
  REQUIRE(ep.symlink("many/link", "f1").ok());
  REQUIRE(ep.mkdir("many/sub", 0750).ok());
  REQUIRE(ep.mkdir("wide", 0755).ok());
  for (int i = 0; i < 3; ++i) put(ep, "wide/f" + std::to_string(i), "x", {1600000000, i});
  for (int i = 0; i < 12; ++i) REQUIRE(ep.mkdir("wide/d" + std::to_string(i), 0755).ok());
  // A subdirectory that nobody may read (EOS gives new directories the mode
  // of their parent).
  Entry secret;
  secret.type = EntryType::Directory;
  secret.mode = 0700;
  REQUIRE(ep.mkdir("wide/secret", 0700).ok());
  REQUIRE(ep.set_metadata("wide/secret", secret, MetaFields::Mode).ok());

  auto nobody = as_nobody(dir.url);
  CHECK_FALSE(nobody->capabilities().can_set_owner);
  StderrCapture err;
  for (const char* d : {"many", "wide"}) {
    INFO("directory ", d);
    auto expected = ep.list(d);
    REQUIRE(expected.ok());
    auto listed = nobody->list(d);
    REQUIRE(listed.ok());
    CHECK(listed.value().entries.size() == expected.value().entries.size());
    for (const Entry& e : expected.value().entries) {
      INFO("entry ", e.name);
      const Entry* got = find(listed.value().entries, e.name);
      REQUIRE(got);
      CHECK(got->type == e.type);
      CHECK(got->size == e.size);
      CHECK(got->mtime == e.mtime);
      CHECK(got->uid == e.uid);
      CHECK(got->gid == e.gid);
      CHECK(got->mode == e.mode);
      CHECK(got->link_target == e.link_target);
    }
  }
  CHECK(err.text().find("cuts find results short") != std::string::npos);

  // Without truncation, the unreadable subdirectory is still listed, and only
  // walking into it fails.
  REQUIRE(ep.mkdir("small", 0755).ok());
  put(ep, "small/f", "f");
  REQUIRE(ep.mkdir("small/secret", 0700).ok());
  REQUIRE(ep.set_metadata("small/secret", secret, MetaFields::Mode).ok());
  auto listed = nobody->list("small");
  REQUIRE(listed.ok());
  CHECK(listed.value().entries.size() == 2);
  REQUIRE(find(listed.value().entries, "secret"));
  CHECK(find(listed.value().entries, "secret")->type == EntryType::Directory);
  auto denied = nobody->list("small/secret");
  REQUIRE_FALSE(denied.ok());
  CHECK(denied.error().kind == ErrorKind::Permission);

  TempDir tmp;
  PosixEndpoint dst(tmp.path());
  SyncOptions options = test_options();
  options.preserve_owner = false;
  Cancellation cancel;
  Report r;
  auto small = as_nobody(dir.url + "/small");
  Engine engine(*small, dst, options, r, nullptr, cancel);
  REQUIRE(engine.run().ok());
  CHECK(r.stats.files_copied == 1);
  REQUIRE(r.failures().size() == 1);
  CHECK(r.failures()[0].path == "secret");
}

// Run as a sudoer by the integration_sudoer test, see tests/CMakeLists.txt.
TEST_CASE("eos endpoint: a sudoer cannot set owners" * doctest::skip()) {
  if (!base_url()) return;
  auto ep = EosEndpoint::create(base_url());
  REQUIRE(ep.ok());
  auto who = ep.value()->proc("mgm.cmd=whoami");
  REQUIRE(who.ok());
  INFO("whoami: ", who.value().out);
  REQUIRE(who.value().out.find("sudo*") != std::string::npos);
  REQUIRE(who.value().out.find("uid=0 ") == std::string::npos);
  CHECK_FALSE(ep.value()->capabilities().can_set_owner);
  CHECK_FALSE(ep.value()->capabilities().symlink_owner);
}

TEST_CASE("eos endpoint: endpoint URLs") {
  if (!base_url()) return;
  // /eos paths go to the instance of the MGM.
  std::string url = base_url();
  std::string mgm = url.substr(0, url.find("//", url.find("://") + 3));
  std::string path = url.substr(mgm.size() + 1);
  EndpointSettings settings;
  settings.mgm = mgm;
  auto ep = make_endpoint(path, settings);
  REQUIRE(ep.ok());
  CHECK(ep.value()->describe() == url);
  CHECK(ep.value()->capabilities().has_symlinks);
  // Runs log where a /eos path took them.
  std::string resolved = describe_resolved(path, *ep.value(), settings);
  CHECK(resolved.find(path + " on ") == 0);
  CHECK(resolved.find(":1094 (" + path + " on the MGM from --mgm)") != std::string::npos);
  EndpointSettings from_env;
  CHECK(describe_resolved(path, *ep.value(), from_env).find("from EOS_MGM_URL") != std::string::npos);
  CHECK(describe_resolved(url, *ep.value(), from_env).find("MGM") == std::string::npos);
  auto below = EosEndpoint::create(url + "/replica2");
  REQUIRE(below.ok());
  CHECK_FALSE(check_disjoint(*below.value(), *ep.value()).ok());
  // Opaque parameters go with every request: as nobody, owners cannot be set.
  auto nobody = make_endpoint(url + "?eos.ruid=65534&eos.rgid=65534", settings);
  REQUIRE(nobody.ok());
  CHECK(nobody.value()->describe() == url);
  CHECK_FALSE(nobody.value()->capabilities().can_set_owner);
  // The kind of server is logged once.
  StderrCapture err;
  CHECK(make_endpoint(path + "/replica2", settings).ok());
  CHECK(err.text().find("EOS instance") == std::string::npos);
}

TEST_CASE("eos endpoint: names that need encoding") {
  if (!base_url()) return;
  RemoteDir dir("names");
  EosEndpoint& ep = *dir.endpoint;
  // Unencoded, these would cut the path short and add opaque parameters;
  // and XrdCl takes x.meta4 for a Metalink document unless told not to.
  for (std::string name :
       {"a b", "q?eos.atomic=0&x=1", "100%25 #1", "Gr\xc3\xb6\xc3\x9f" "e", "x.meta4"}) {
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
    REQUIRE(listed.value().entries.size() == 1);
    CHECK(listed.value().entries[0].name == name);
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

  // Symlinks carry their path and target as they are.
  for (auto [link, target] :
       {std::pair{"link with spaces", "target with spaces"},
        std::pair{"l?x=1&y", "t&mgm.file.target=/x?eos.ruid=0"},
        std::pair{"100%25 #1", "100% \"quoted\" 'single' #frag"},
        std::pair{"l", "../a=b&c=d"}}) {
    INFO("link ", link, " to ", target);
    REQUIRE(ep.symlink(link, target).ok());
    CHECK(must(ep.stat(link)).link_target == target);
    auto listed = ep.list("");
    REQUIRE(listed.ok());
    REQUIRE(find(listed.value().entries, link));
    CHECK(find(listed.value().entries, link)->link_target == target);
    REQUIRE(ep.remove(link, EntryType::Symlink).ok());
  }
  // What EOS would change or could not list back is refused.
  for (auto [link, target] : {std::pair{"a#AND#b", "t"}, std::pair{"l", "fid:12"},
                              std::pair{"l", "line\nbreak"}}) {
    auto refused = ep.symlink(link, target);
    REQUIRE_FALSE(refused.ok());
    CHECK(refused.error().kind == ErrorKind::Unsupported);
  }
  // Names that make a listing line ambiguous, or have control characters,
  // are left out of listings and counted.
  for (std::string name : {"amb\" type=file size=1", "tab\there"}) {
    INFO("name ", name);
    put(ep, name, "x");
    auto listed = ep.list("");
    REQUIRE(listed.ok());
    CHECK(listed.value().entries.empty());
    CHECK(listed.value().skipped == 1);
    REQUIRE(ep.remove(name, EntryType::File).ok());
  }
  CHECK(ep.list("").value().entries.empty());
}

TEST_CASE("eos endpoint: symlinks are replaced in place") {
  if (!base_url()) return;
  RemoteDir dir("relink");
  EosEndpoint& ep = *dir.endpoint;
  REQUIRE(ep.mkdir("d", 0755).ok());
  put(ep, "d/f", "content");
  Entry before = must(ep.stat("d"));
  // Over a dangling symlink, one to a directory and one to a file.
  REQUIRE(ep.symlink("l", "/nonexistent").ok());
  REQUIRE(ep.symlink("l", "d").ok());
  CHECK(must(ep.stat("l")).link_target == "d");
  REQUIRE(ep.symlink("l", "d/f").ok());
  CHECK(must(ep.stat("l")).link_target == "d/f");
  CHECK(get(ep, "d/f") == "content");
  CHECK(must(ep.stat("d")).type == EntryType::Directory);

  // Owners of a symlink are its own, not those of its target.
  REQUIRE(ep.symlink("l", "d").ok());
  Entry md;
  md.type = EntryType::Symlink;
  md.uid = 4444;
  md.gid = 5555;
  REQUIRE(ep.set_metadata("l", md, MetaFields::Owner).ok());
  CHECK(must(ep.stat("l")).uid == 4444);
  CHECK(must(ep.stat("l")).gid == 5555);
  CHECK(must(ep.stat("d")).uid == before.uid);
  CHECK(must(ep.stat("d")).gid == before.gid);
  CHECK(ep.list("").value().entries.size() == 2);
}

TEST_CASE("FS to EOS as a user other than root: read-only directories") {
  if (!base_url()) return;
  RemoteDir dir("readonly");
  Entry nobody_owns;
  nobody_owns.type = EntryType::Directory;
  nobody_owns.uid = 65534;
  nobody_owns.gid = 65534;
  REQUIRE(dir.endpoint->set_metadata("", nobody_owns, MetaFields::Owner).ok());
  auto nobody = as_nobody(dir.url);

  TempDir tmp;
  tmp.write_file("src/ro/f", "x", 0444);
  tmp.write_file("src/ro/sub/g", "y", 0444);
  for (const char* d : {"src/ro/sub", "src/ro"}) REQUIRE(chmod(tmp.sub(d).c_str(), 0555) == 0);
  PosixEndpoint src(tmp.sub("src"));
  SyncOptions options = test_options();
  options.preserve_owner = false;
  options.delete_extra = true;
  auto run = [&] {
    Cancellation cancel;
    Report r;
    Engine engine(src, *nobody, options, r, nullptr, cancel);
    REQUIRE(engine.run().ok());
    CHECK(r.stats.failures == 0);
    return r.stats.files_copied.load();
  };
  CHECK(run() == 2);
  CHECK(must(nobody->stat("ro")).mode == 0555);
  CHECK(must(nobody->stat("ro/sub")).mode == 0555);

  // EOS sets the mtime of a directory only with write access to it, and a
  // read-only tree is deleted only after making it writable.
  struct timespec times[2] = {{0, UTIME_OMIT}, {1600000000, 5}};
  REQUIRE(utimensat(AT_FDCWD, tmp.sub("src/ro/sub").c_str(), times, 0) == 0);
  for (const char* d : {"ro/extra", "ro/extra/inner"}) {
    REQUIRE(dir.endpoint->mkdir(d, 0755).ok());
    REQUIRE(dir.endpoint->set_metadata(d, nobody_owns, MetaFields::Owner).ok());
  }
  Entry read_only = nobody_owns;
  read_only.mode = 0555;
  for (const char* d : {"ro/extra/inner", "ro/extra"})
    REQUIRE(dir.endpoint->set_metadata(d, read_only, MetaFields::Mode).ok());
  CHECK(run() == 0);
  CHECK(must(nobody->stat("ro/sub")).mtime == Timespec{1600000000, 5});
  CHECK(must(nobody->stat("ro/sub")).mode == 0555);
  CHECK(must(nobody->stat("ro")).mode == 0555);
  CHECK(nobody->stat("ro/extra").error().kind == ErrorKind::NotFound);
}

TEST_CASE("FS to EOS: mode bits that EOS does not store") {
  if (!base_url()) return;
  TempDir tmp;
  tmp.write_file("src/setuid", "s", 04755);
  tmp.write_file("src/d/f", "f", 0640);
  chmod(tmp.sub("src/d").c_str(), 06750);
  PosixEndpoint src(tmp.sub("src"));
  RemoteDir dir("modes");
  SyncOptions options = test_options();
  options.preserve_owner = false;
  Cancellation cancel;
  Report r;
  Engine engine(src, *dir.endpoint, options, r, nullptr, cancel);
  REQUIRE(engine.run().ok());
  CHECK(r.stats.failures == 0);
  CHECK(must(dir.endpoint->stat("setuid")).mode == 0755);
  CHECK(must(dir.endpoint->stat("d")).mode == 02750);
  Report r2;
  Engine again(src, *dir.endpoint, options, r2, nullptr, cancel);
  REQUIRE(again.run().ok());
  CHECK(r2.stats.metadata_fixed == 0);
  CHECK(r2.stats.files_copied == 0);
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
    CHECK(r4.stats.files_unverified == 0);
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

TEST_CASE("FS to EOS: pipelined writes give erasure-coded files their parity") {
  if (!base_url()) return;
  RemoteDir dir("parity", "raid6");
  EosEndpoint& ep = *dir.endpoint;
  REQUIRE(ep.target_note().find("xrootd.async off") != std::string::npos);
  XrdOptions one_by_one;
  one_by_one.write_window = 1;
  CHECK(EosEndpoint::create(dir.url, one_by_one).value()->target_note().empty());

  // Files of several RAIN groups (4 data stripes of 1 MiB), copied in many
  // chunks with the default write window.
  TempDir tmp;
  std::string content = pattern(9 * 1024 * 1024 + 12345);
  for (int i = 0; i < 3; ++i) tmp.write_file("src/f" + std::to_string(i), content);
  PosixEndpoint src(tmp.sub("src"));
  SyncOptions options = test_options();
  options.buffer_size = 256 * 1024;
  Cancellation cancel;
  Report r;
  Engine engine(src, ep, options, r, nullptr, cancel);
  REQUIRE(engine.run().ok());
  REQUIRE(r.stats.files_copied == 3);

  // Data stripes come first, parity stripes last. Each file loses two
  // stripes, another pair each: what is missing of the data must come from
  // the parity, which an out-of-order write leaves zero (EOS up to 5.5.2).
  auto locations = [&](const std::string& path) {
    auto info = ep.proc("mgm.cmd=fileinfo&mgm.path=" + ep.absolute(path) +
                        "&mgm.file.info.option=-m");
    REQUIRE(info.ok());
    REQUIRE(info.value().out.find(" layout=raid6 nstripes=6 ") != std::string::npos);
    std::vector<std::string> fsids;
    for (size_t at = info.value().out.find(" fsid="); at != std::string::npos;
         at = info.value().out.find(" fsid=", at + 1)) {
      std::string rest = info.value().out.substr(at + 6);
      fsids.push_back(rest.substr(0, rest.find_first_not_of("0123456789")));
    }
    return fsids;
  };
  for (int i = 0; i < 3; ++i) {
    std::string path = "f" + std::to_string(i);
    INFO("file ", path);
    auto fsids = locations(path);
    REQUIRE(fsids.size() == 6);
    for (int stripe : {2 * i, 2 * i + 1}) {
      auto dropped = ep.proc("mgm.cmd=file&mgm.subcmd=drop&mgm.path=" + ep.absolute(path) +
                             "&mgm.file.fsid=" + fsids[static_cast<size_t>(stripe)]);
      REQUIRE(dropped.ok());
      REQUIRE(dropped.value().retc == 0);
    }
    REQUIRE(locations(path).size() == 4);
    CHECK(get(ep, path) == content);
  }
}

TEST_CASE("FS to EOS and back over one shared connection") {
  if (!base_url()) return;
  TempDir tmp;
  for (int i = 0; i < 6; ++i) tmp.write_file("src/f" + std::to_string(i), pattern(300000 + i));
  PosixEndpoint src(tmp.sub("src"));
  RemoteDir dir("connections");
  XrdOptions xo;
  xo.shared_connection = true;
  auto dst = EosEndpoint::create(dir.url, xo);
  REQUIRE(dst.ok());
  SyncOptions options = test_options();
  options.preserve_owner = false;
  Cancellation cancel;
  Report r;
  Engine engine(src, *dst.value(), options, r, nullptr, cancel);
  REQUIRE(engine.run().ok());
  CHECK(r.stats.files_copied == 6);
  CHECK(r.stats.files_unverified == 0);
  CHECK(r.stats.failures == 0);

  PosixEndpoint back(tmp.sub("back"));
  Report r2;
  Engine restore(*dst.value(), back, options, r2, nullptr, cancel);
  REQUIRE(restore.run().ok());
  CHECK(r2.stats.files_copied == 6);
  CHECK(r2.stats.files_unverified == 0);
  for (int i = 0; i < 6; ++i)
    CHECK(tmp.read_file("back/f" + std::to_string(i)) == pattern(300000 + i));
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

  // Into a directory without checksums, the source's listed checksum
  // verifies the copy, which meets --require-checksum.
  RemoteDir c("eos2eos-nochecksum", "nochecksum");
  options.require_checksum = true;
  Report r2;
  Engine strict(*a.endpoint, *c.endpoint, options, r2, nullptr, cancel);
  REQUIRE(strict.run().ok());
  CHECK(r2.stats.files_copied == 1);
  CHECK(r2.stats.files_unverified == 0);
  CHECK(r2.stats.failures == 0);
  CHECK(get(*c.endpoint, "f") == content);
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
    CHECK(dir.endpoint->list("").value().entries.empty());
  }
}
