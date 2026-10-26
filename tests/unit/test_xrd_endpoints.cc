// SPDX-License-Identifier: GPL-3.0-or-later
//
// The XRootD and EOS endpoints on a fake XRootD client (fake_xrd_client.hh):
// what they send and what the instance ends up with, without a server.
#include <algorithm>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "doctest/doctest.h"
#include "eosmirror/checksum.hh"
#include "eosmirror/eos_endpoint.hh"
#include "eosmirror/xrootd_endpoint.hh"
#include "fake_xrd_client.hh"

using namespace eosmirror;

namespace {

// An instance with the directory /eos/d, which kEosUrl names.
constexpr const char* kEosUrl = "root://mgm.fake//eos/d";

std::shared_ptr<FakeXrd> instance() {
  auto xrd = std::make_shared<FakeXrd>();
  xrd->dir("/eos/d");
  return xrd;
}

std::unique_ptr<EosEndpoint> eos(const std::shared_ptr<FakeXrd>& xrd,
                                 const std::string& url = kEosUrl) {
  auto ep = EosEndpoint::create(url, XrdOptions(), xrd);
  REQUIRE(ep.ok());
  return std::move(ep).value();
}

bool logged(const std::shared_ptr<FakeXrd>& xrd, std::string_view request) {
  auto sent = xrd->requests();
  return std::find(sent.begin(), sent.end(), request) != sent.end();
}

// Writes content in chunks of the pool's buffer size, several in flight.
Status write_all(FileWriter& writer, std::string_view content, BufferPool& pool) {
  for (size_t offset = 0; offset < content.size(); offset += pool.buffer_size()) {
    Chunk chunk{offset, std::min(pool.buffer_size(), content.size() - offset), pool.acquire()};
    std::memcpy(chunk.buffer.span().data(), content.data() + offset, chunk.size);
    if (Status s = writer.write(std::move(chunk)); !s.ok()) return s;
  }
  return {};
}

// The whole file, read in chunks of the pool's buffer size.
Result<std::string> read_all(FileReader& reader, uint64_t size, BufferPool& pool) {
  std::string out;
  while (out.size() < size) {
    auto chunk = reader.read_chunk(out.size(), size, pool);
    if (!chunk.ok()) return chunk.error();
    if (chunk.value().size == 0) break;
    out.append(reinterpret_cast<const char*>(chunk.value().buffer.span().data()),
               chunk.value().size);
  }
  return out;
}

CommitSpec spec_for(std::string_view content) {
  CommitSpec spec;
  spec.size = content.size();
  spec.metadata.type = EntryType::File;
  spec.metadata.mode = 0640;
  spec.metadata.uid = 1001;
  spec.metadata.gid = 1002;
  spec.metadata.mtime = {1700000000, 123456789};
  Hasher h(ChecksumType::Adler32);
  h.update({reinterpret_cast<const std::byte*>(content.data()), content.size()});
  spec.checksum = h.finish();
  return spec;
}

bool hidden_uploads(const std::shared_ptr<FakeXrd>& xrd) {
  auto paths = xrd->paths();
  return std::any_of(paths.begin(), paths.end(),
                     [](const std::string& p) { return p.find("/.sys.a#.") != std::string::npos; });
}

}  // namespace

TEST_CASE("EOS endpoints on the fake: the identity and the root are settled on creation") {
  auto xrd = instance();
  xrd->symlink("/eos/link", "/eos/d");
  SUBCASE("as root") {
    auto ep = eos(xrd, "root://mgm.fake//eos/link");
    CHECK(xrd->requests().front() == "proc mgm.cmd=whoami");
    CHECK(ep->location().path == "/eos/d");
    CHECK(ep->describe() == "root://mgm.fake//eos/link");
    Capabilities caps = ep->capabilities();
    CHECK(caps.mtime_resolution == 1);
    CHECK(caps.has_owners);
    CHECK(caps.can_set_owner);
    CHECK(caps.can_set_mode);
    CHECK(caps.file_mode_bits == 0777);
    CHECK(caps.dir_mode_bits == 03777);
    CHECK(caps.can_set_mtime);
    CHECK(caps.has_symlinks);
    CHECK_FALSE(caps.follows_symlinks);
    CHECK(caps.symlink_owner);
    CHECK(caps.checksum == ChecksumType::Adler32);
    CHECK(caps.confirms_after_commit);
  }
  SUBCASE("as another user, who may not set owners") {
    xrd->root = false;
    Capabilities caps = eos(xrd)->capabilities();
    CHECK_FALSE(caps.can_set_owner);
    CHECK_FALSE(caps.symlink_owner);
    CHECK(caps.has_owners);
  }
}

TEST_CASE("EOS endpoints on the fake: stat") {
  auto xrd = instance();
  uint64_t fid = xrd->file("/eos/d/f", "content", {1600000000, 5}, 0640);
  xrd->symlink("/eos/d/l", "../t a");
  auto ep = eos(xrd);

  auto f = ep->stat("f");
  REQUIRE(f.ok());
  CHECK(f.value().name == "f");
  CHECK(f.value().type == EntryType::File);
  CHECK(f.value().size == 7);
  CHECK(f.value().id == std::to_string(fid));
  CHECK(f.value().mode == 0640);
  CHECK(f.value().mtime == Timespec{1600000000, 5});

  auto root = ep->stat("");
  REQUIRE(root.ok());
  CHECK(root.value().type == EntryType::Directory);
  CHECK(root.value().name.empty());

  auto l = ep->stat("l");
  REQUIRE(l.ok());
  CHECK(l.value().type == EntryType::Symlink);
  CHECK(l.value().link_target == "../t a");

  auto missing = ep->stat("missing");
  REQUIRE_FALSE(missing.ok());
  CHECK(missing.error().kind == ErrorKind::NotFound);
}

TEST_CASE("EOS endpoints on the fake: an upload is in place, confirmed by its file's id") {
  auto xrd = instance();
  uint64_t old_fid = 0;
  SUBCASE("a new file") {}
  SUBCASE("replacing a file") {
    old_fid = xrd->file("/eos/d/f", "old");
  }
  auto ep = eos(xrd);
  std::string content = "eleven characters and more";
  CommitSpec spec = spec_for(content);
  BufferPool pool(4);
  auto writer = ep->open_write("f", spec);
  REQUIRE(writer.ok());
  REQUIRE(write_all(*writer.value(), content, pool).ok());
  auto committed = writer.value()->commit(spec);
  REQUIRE(committed.ok());
  CHECK(committed.value().verified);
  writer.value().reset();

  auto f = xrd->at("/eos/d/f");
  REQUIRE(f);
  CHECK(f->content == content);
  CHECK(f->fid != old_fid);
  CHECK(f->mtime == spec.metadata.mtime);  // carried by the open
  CHECK(f->mode == 0640);
  CHECK(f->uid == 1001);
  CHECK(f->gid == 1002);
  CHECK_FALSE(hidden_uploads(xrd));
  CHECK(logged(xrd, "query:getfmd /eos/d/f"));
}

TEST_CASE("EOS endpoints on the fake: reads") {
  auto xrd = instance();
  std::string content = "0123456789abcdefghij";
  xrd->file("/eos/d/f", content);
  auto ep = eos(xrd);
  auto listed = ep->stat("f");
  REQUIRE(listed.ok());
  BufferPool pool(3);
  auto reader = ep->open_read("f");
  REQUIRE(reader.ok());
  auto state = reader.value()->state(listed.value());
  REQUIRE(state.ok());
  CHECK(read_all(*reader.value(), content.size(), pool).value() == content);
  auto unchanged = reader.value()->unchanged(state.value());
  REQUIRE(unchanged.ok());
  CHECK(unchanged.value());
}

TEST_CASE("XRootD endpoints on the fake: a file is not moved aside where symlinks may be") {
  // Moving a symlink aside renames what it points to, and removing it then
  // deletes that: without --target-without-symlinks the upload is refused.
  auto xrd = std::make_shared<FakeXrd>();
  xrd->dir("/data");
  xrd->file("/data/f", "old");
  auto created = XrdEndpoint::create("root://xrd.fake//data", XrdOptions(), xrd);
  REQUIRE(created.ok());
  std::string content = "plain xrootd";
  CommitSpec spec = spec_for(content);
  BufferPool pool(5);
  auto writer = created.value()->open_write("f", spec);
  REQUIRE(writer.ok());
  REQUIRE(write_all(*writer.value(), content, pool).ok());
  auto committed = writer.value()->commit(spec);
  REQUIRE_FALSE(committed.ok());
  CHECK(committed.error().kind == ErrorKind::Unsupported);
  CHECK(committed.error().message.find("--target-without-symlinks") != std::string::npos);
  CHECK(writer.value()->abort().ok());
  writer.value().reset();
  REQUIRE(xrd->at("/data/f"));
  CHECK(xrd->at("/data/f")->content == "old");
  CHECK(xrd->paths() == std::vector<std::string>{"/", "/data", "/data/f"});
}

TEST_CASE("XRootD endpoints on the fake: uploads are renamed into place, verified") {
  auto xrd = std::make_shared<FakeXrd>();
  xrd->dir("/data");
  bool replacing = false;
  XrdOptions options;
  SUBCASE("a new file") {}
  SUBCASE("replacing a file, which the server refuses to move onto") {
    // Moved aside only in a tree stated to have no symlinks.
    replacing = true;
    options.without_symlinks = true;
    xrd->file("/data/f", "old");
  }
  auto created = XrdEndpoint::create("root://xrd.fake//data", options, xrd);
  REQUIRE(created.ok());
  auto& ep = *created.value();
  CHECK(ep.capabilities().checksum == ChecksumType::Adler32);
  CHECK_FALSE(ep.capabilities().can_set_mtime);

  std::string content = "plain xrootd";
  CommitSpec spec = spec_for(content);
  BufferPool pool(5);
  auto writer = ep.open_write("f", spec);
  REQUIRE(writer.ok());
  REQUIRE(write_all(*writer.value(), content, pool).ok());
  auto committed = writer.value()->commit(spec);
  REQUIRE(committed.ok());
  CHECK(committed.value().verified);
  writer.value().reset();

  auto f = xrd->at("/data/f");
  REQUIRE(f);
  CHECK(f->content == content);
  CHECK(xrd->paths() == std::vector<std::string>{"/", "/data", "/data/f"});
  auto sent = xrd->requests();
  bool moved_aside = std::any_of(sent.begin(), sent.end(), [](const std::string& r) {
    return r.starts_with("mv /data/f /data/");
  });
  CHECK(moved_aside == replacing);
}
