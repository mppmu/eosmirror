// SPDX-License-Identifier: GPL-3.0-or-later
//
// Integration tests against a live XRootD server. EOSMIRROR_XROOTD_URL names
// a writable directory on it, e.g. root://xrootd:1094//data; the tests use a
// fresh subdirectory of it.
#include <sys/stat.h>
#include <unistd.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>

#include "doctest/doctest.h"
#include "eosmirror/engine.hh"
#include "eosmirror/posix_endpoint.hh"
#include "eosmirror/selftest.hh"
#include "eosmirror/xrootd_endpoint.hh"
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
  const char* url = std::getenv("EOSMIRROR_XROOTD_URL");
  return url && *url ? url : nullptr;
}

// A fresh directory under the base URL for one test case.
struct RemoteDir {
  explicit RemoteDir(const std::string& label) {
    std::string base = base_url();
    auto parent = XrdEndpoint::create(base);
    REQUIRE(parent.ok());
    name = label + "-" + std::to_string(getpid()) + "-" +
           std::to_string(std::chrono::steady_clock::now().time_since_epoch().count() % 100000);
    REQUIRE(parent.value()->mkdir(name, 0755).ok());
    url = base + "/" + name;
    auto ep = XrdEndpoint::create(url);
    REQUIRE(ep.ok());
    endpoint = std::move(ep).value();
  }

  std::string name;
  std::string url;
  std::unique_ptr<XrdEndpoint> endpoint;
};

SyncOptions test_options() {
  SyncOptions o;
  o.checkers = 2;
  o.transfers = 3;
  o.buffer_size = 64 * 1024;
  o.retry.attempts = 2;
  o.retry.initial_delay = std::chrono::milliseconds(10);
  o.preserve_owner = false;
  return o;
}

}  // namespace

TEST_CASE("xrootd endpoint: stat, list, mkdir, write, checksum, rename, remove") {
  if (!base_url()) return;
  RemoteDir dir("endpoint");
  XrdEndpoint& ep = *dir.endpoint;
  Capabilities caps = ep.capabilities();
  CHECK(caps.checksum == ChecksumType::Adler32);
  CHECK_FALSE(caps.can_set_owner);
  CHECK_FALSE(caps.can_set_mtime);
  CHECK_FALSE(caps.has_symlinks);
  CHECK(caps.mtime_resolution == 1000000000);

  auto root = ep.stat("");
  REQUIRE(root.ok());
  CHECK(root.value().type == EntryType::Directory);
  CHECK(ep.list("").value().empty());
  CHECK(ep.stat("missing").error().kind == ErrorKind::NotFound);

  REQUIRE(ep.mkdir("d", 0750).ok());
  // Plain xrootd reports success for an existing directory, EOS an error.
  Status again = ep.mkdir("d", 0750);
  CHECK((again.ok() || again.error().kind == ErrorKind::Exists));

  std::string content = pattern(200000);
  CommitSpec spec;
  spec.size = content.size();
  spec.metadata.mode = 0640;
  spec.fields = MetaFields::Mode;
  Hasher hasher(ChecksumType::Adler32);
  hasher.update(bytes(content));
  spec.checksum = hasher.finish();

  auto writer = ep.open_write("d/f", spec);
  REQUIRE(writer.ok());
  for (size_t off = 0; off < content.size(); off += 70000) {
    size_t n = std::min<size_t>(70000, content.size() - off);
    REQUIRE(writer.value()->write(off, bytes(std::string_view(content).substr(off, n))).ok());
  }
  // Only the temporary is visible until the commit.
  auto during = ep.list("d");
  REQUIRE(during.ok());
  REQUIRE(during.value().size() == 1);
  CHECK(ep.is_temporary(during.value()[0].name));
  REQUIRE(writer.value()->commit(spec).ok());

  auto listed = ep.list("d");
  REQUIRE(listed.ok());
  REQUIRE(listed.value().size() == 1);
  CHECK(listed.value()[0].name == "f");
  CHECK(listed.value()[0].type == EntryType::File);
  CHECK(listed.value()[0].size == content.size());
  CHECK((ep.query_checksum(ep.absolute("d/f")).value() == spec.checksum));

  // Reading back.
  auto reader = ep.open_read("d/f");
  REQUIRE(reader.ok());
  std::string back(content.size(), '\0');
  auto n = reader.value()->read(0, {reinterpret_cast<std::byte*>(back.data()), back.size()});
  REQUIRE(n.ok());
  CHECK(n.value() == content.size());
  CHECK(back == content);
  CHECK(reader.value()->stat().value().size == content.size());

  // A wrong checksum is rejected and leaves nothing behind.
  CommitSpec bad = spec;
  bad.checksum.hex = "00000000";
  auto w2 = ep.open_write("d/g", bad);
  REQUIRE(w2.ok());
  REQUIRE(w2.value()->write(0, bytes(content)).ok());
  auto s = w2.value()->commit(bad);
  REQUIRE_FALSE(s.ok());
  CHECK(s.error().kind == ErrorKind::Checksum);
  CHECK(ep.list("d").value().size() == 1);

  // Committing over an existing file replaces it.
  std::string other = "replacement";
  CommitSpec spec2;
  spec2.size = other.size();
  Hasher h2(ChecksumType::Adler32);
  h2.update(bytes(other));
  spec2.checksum = h2.finish();
  auto w3 = ep.open_write("d/f", spec2);
  REQUIRE(w3.ok());
  REQUIRE(w3.value()->write(0, bytes(other)).ok());
  REQUIRE(w3.value()->commit(spec2).ok());
  CHECK(ep.stat("d/f").value().size == other.size());

  CHECK(ep.symlink("d/l", "f").error().kind == ErrorKind::Unsupported);
  ErrorKind not_empty = ep.remove("d", EntryType::Directory).error().kind;
  CHECK((not_empty == ErrorKind::NotEmpty || not_empty == ErrorKind::Exists));  // EEXIST is POSIX too
  REQUIRE(ep.remove("d/f", EntryType::File).ok());
  REQUIRE(ep.remove("d", EntryType::Directory).ok());
  CHECK(ep.list("").value().empty());
}

TEST_CASE("FS to xrootd and back") {
  if (!base_url()) return;
  TempDir tmp;
  tmp.write_file("src/a.txt", "hello", 0640);
  tmp.write_file("src/big.bin", pattern(300 * 1024 + 17), 0600);
  tmp.write_file("src/empty", "", 0444);
  tmp.write_file("src/sub/nested/file", pattern(1000), 0664);
  REQUIRE(symlink("a.txt", tmp.sub("src/link").c_str()) == 0);
  PosixEndpoint src(tmp.sub("src"));
  RemoteDir dir("sync");
  XrdEndpoint& dst = *dir.endpoint;

  Cancellation cancel;
  Report r;
  Engine engine(src, dst, test_options(), r, nullptr, cancel);
  REQUIRE(engine.run().ok());
  CHECK(r.stats.files_copied == 4);
  CHECK(r.stats.dirs_created == 2);
  CHECK(r.stats.failures == 0);
  CHECK(r.stats.symlinks_skipped == 1);
  CHECK(dst.stat("sub/nested/file").value().size == 1000);

  // Without mtimes on the target, a rerun compares sizes only.
  Report r2;
  Engine again(src, dst, test_options(), r2, nullptr, cancel);
  REQUIRE(again.run().ok());
  CHECK(r2.stats.files_copied == 0);
  CHECK(r2.stats.files_unchanged == 4);
  CHECK(r2.stats.failures == 0);

  tmp.write_file("src/a.txt", "hello world", 0640);
  Report r3;
  Engine changed(src, dst, test_options(), r3, nullptr, cancel);
  REQUIRE(changed.run().ok());
  CHECK(r3.stats.files_copied == 1);

  // Back to a local directory: everything comes back, sizes and content.
  PosixEndpoint back(tmp.sub("back"));
  Report r4;
  Engine restore(dst, back, test_options(), r4, nullptr, cancel);
  REQUIRE(restore.run().ok());
  CHECK(r4.stats.files_copied == 4);
  CHECK(r4.stats.failures == 0);
  CHECK(tmp.read_file("back/a.txt") == "hello world");
  CHECK(tmp.read_file("back/big.bin") == tmp.read_file("src/big.bin"));
  CHECK(tmp.read_file("back/sub/nested/file") == pattern(1000));
}

TEST_CASE("selftest against xrootd") {
  if (!base_url()) return;
  RemoteDir dir("selftest");
  SelftestReport r = run_selftest(*dir.endpoint, false);
  for (const SelftestCheck& c : r.checks) {
    INFO(c.name, ": ", c.message);
    CHECK(c.ok);
  }
  CHECK(r.ok());
  CHECK(dir.endpoint->list("").value().empty());
}
