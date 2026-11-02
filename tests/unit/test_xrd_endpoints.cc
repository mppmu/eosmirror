// SPDX-License-Identifier: GPL-3.0-or-later
//
// The XRootD and EOS endpoints on a fake XRootD client (fake_xrd_client.hh):
// what they send and what the instance ends up with, without a server.
#include <algorithm>
#include <cerrno>
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

// Uploads content and commits it. The writer is gone afterwards, so that an
// abort at its destruction has happened as well.
Result<CommitInfo> upload(Endpoint& ep, const RelPath& path, std::string_view content,
                          const CommitSpec& spec) {
  BufferPool pool(4);
  auto writer = ep.open_write(path, spec);
  if (!writer.ok()) return writer.error();
  if (Status s = write_all(*writer.value(), content, pool); !s.ok()) return s.error();
  return writer.value()->commit(spec);
}

std::string proc_reply(const std::string& out, const std::string& err = "", int retc = 0) {
  return "mgm.proc.stdout=" + out + "&mgm.proc.stderr=" + err +
         "&mgm.proc.retc=" + std::to_string(retc);
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

TEST_CASE("EOS uploads whose close fails although EOS committed them are taken back") {
  auto xrd = instance();
  std::string content = "the new content";
  CommitSpec spec = spec_for(content);
  // The storage node commits the upload; the client hears of no success.
  xrd->fail(
      {.op = "close", .path = "/eos/d/f", .status = FakeXrd::timeout(), .takes_effect = true});

  SUBCASE("replacing a file: kept with mtime 0, so that EOS keeps its versions") {
    xrd->file("/eos/d/f", "old");
    auto committed = upload(*eos(xrd), "f", content, spec);
    REQUIRE_FALSE(committed.ok());
    CHECK_FALSE(committed.error().in_doubt);
    auto f = xrd->at("/eos/d/f");
    REQUIRE(f);
    CHECK(f->content == content);
    CHECK(f->mtime == Timespec{0, 0});
  }
  SUBCASE("replacing nothing: removed") {
    spec.replaces = false;
    auto committed = upload(*eos(xrd), "f", content, spec);
    REQUIRE_FALSE(committed.ok());
    CHECK_FALSE(committed.error().in_doubt);
    CHECK_FALSE(xrd->at("/eos/d/f"));
  }
  SUBCASE("where it can be neither reset nor removed: in place, in doubt") {
    xrd->file("/eos/d/f", "old");
    xrd->fail({.op = "query:utimes", .status = FakeXrd::error(kXR_NotAuthorized, "no")});
    xrd->fail({.op = "rm", .status = FakeXrd::error(kXR_NotAuthorized, "no")});
    auto committed = upload(*eos(xrd), "f", content, spec);
    REQUIRE_FALSE(committed.ok());
    CHECK(committed.error().in_doubt);
    CHECK(committed.error().kind == ErrorKind::IO);
    auto f = xrd->at("/eos/d/f");
    REQUIRE(f);
    CHECK(f->content == content);
    CHECK(f->mtime == spec.metadata.mtime);
  }
  CHECK_FALSE(hidden_uploads(xrd));
}

TEST_CASE("EOS uploads: the file in place is told by its id") {
  auto xrd = instance();
  std::string content = "the new content";
  CommitSpec spec = spec_for(content);

  SUBCASE("replaced by another writer before the confirmation: left alone") {
    xrd->on_commit = [&](const std::string& path) { xrd->file(path, "another writer's"); };
    auto committed = upload(*eos(xrd), "f", content, spec);
    REQUIRE_FALSE(committed.ok());
    CHECK_FALSE(committed.error().in_doubt);
    CHECK(xrd->at("/eos/d/f")->content == "another writer's");
  }
  SUBCASE("moved away, another file under its name, which EOS writes as the other's") {
    // EOS writes '&' as "#AND#" in replies: the upload, moved to a literal
    // "a#AND#b", is reported under the name it was written to.
    xrd->on_commit = [&](const std::string& path) {
      xrd->rename(path, "/eos/d/a#AND#b");
      xrd->file(path, "another writer's");
    };
    auto committed = upload(*eos(xrd), "a&b", content, spec);
    REQUIRE_FALSE(committed.ok());
    CHECK_FALSE(committed.error().in_doubt);
    CHECK(xrd->at("/eos/d/a&b")->content == "another writer's");
    CHECK(xrd->at("/eos/d/a#AND#b")->content == content);
  }
  SUBCASE("a close reported done after the connection was lost: not in place") {
    xrd->file("/eos/d/f", "old");
    // The storage node discards the upload; XrdCl reports the close as done.
    xrd->fail({.op = "close", .path = "/eos/d/f"});
    auto committed = upload(*eos(xrd), "f", content, spec);
    REQUIRE_FALSE(committed.ok());
    CHECK_FALSE(committed.error().in_doubt);
    CHECK(xrd->at("/eos/d/f")->content == "old");
  }
  CHECK_FALSE(hidden_uploads(xrd));
}

TEST_CASE("EOS uploads whose confirmation fails") {
  auto xrd = instance();
  xrd->file("/eos/d/f", "old");
  std::string content = "the new content";
  CommitSpec spec = spec_for(content);

  SUBCASE("getfmd times out once: taken back, not in doubt") {
    xrd->fail({.op = "query:getfmd", .status = FakeXrd::timeout()});
    auto committed = upload(*eos(xrd), "f", content, spec);
    REQUIRE_FALSE(committed.ok());
    CHECK(committed.error().kind == ErrorKind::Timeout);
    CHECK_FALSE(committed.error().in_doubt);
    CHECK(xrd->at("/eos/d/f")->mtime == Timespec{0, 0});
  }
  SUBCASE("getfmd keeps failing: left in place, in doubt") {
    xrd->fail({.op = "query:getfmd", .status = FakeXrd::timeout(), .remaining = 10});
    auto committed = upload(*eos(xrd), "f", content, spec);
    REQUIRE_FALSE(committed.ok());
    CHECK(committed.error().in_doubt);
    CHECK(committed.error().kind == ErrorKind::IO);
    auto f = xrd->at("/eos/d/f");
    CHECK(f->content == content);
    CHECK(f->mtime == spec.metadata.mtime);
  }
}

TEST_CASE("XRootD uploads whose checksum query fails leave the target as it was") {
  auto xrd = std::make_shared<FakeXrd>();
  xrd->dir("/data");
  xrd->file("/data/f", "old");
  auto ep = XrdEndpoint::create("root://xrd.fake//data", XrdOptions(), xrd);
  REQUIRE(ep.ok());
  xrd->fail({.op = "query:checksum", .status = FakeXrd::timeout()});
  std::string content = "plain xrootd";
  auto committed = upload(*ep.value(), "f", content, spec_for(content));
  REQUIRE_FALSE(committed.ok());
  CHECK(committed.error().kind == ErrorKind::Timeout);
  CHECK(xrd->paths() == std::vector<std::string>{"/", "/data", "/data/f"});
  CHECK(xrd->at("/data/f")->content == "old");
}

TEST_CASE("EOS uploads: getfmd replies that cannot be relied on") {
  auto xrd = instance();
  xrd->file("/eos/d/f", "old");
  std::string content = "the new content";
  CommitSpec spec = spec_for(content);

  SUBCASE("a reply of another form: left in place, in doubt") {
    xrd->reply = [](std::string_view pcmd, const std::string&,
                    const std::string&) -> std::optional<std::string> {
      if (pcmd != "getfmd") return std::nullopt;
      return "getfmd: retc=0 name=f";
    };
    auto committed = upload(*eos(xrd), "f", content, spec);
    REQUIRE_FALSE(committed.ok());
    CHECK(committed.error().in_doubt);
    CHECK(xrd->at("/eos/d/f")->content == content);
  }
  SUBCASE("another size than written: taken back, not in doubt") {
    xrd->reply = [](std::string_view pcmd, const std::string&,
                    const std::string& reply) -> std::optional<std::string> {
      auto size = reply.find("&size=");
      if (pcmd != "getfmd" || size == std::string::npos) return std::nullopt;
      return reply.substr(0, size) + "&size=1" + reply.substr(reply.find('&', size + 1));
    };
    auto committed = upload(*eos(xrd), "f", content, spec);
    REQUIRE_FALSE(committed.ok());
    CHECK(committed.error().kind == ErrorKind::IO);
    CHECK_FALSE(committed.error().in_doubt);
    CHECK(xrd->at("/eos/d/f")->mtime == Timespec{0, 0});
  }
  CHECK_FALSE(hidden_uploads(xrd));
}

TEST_CASE("EOS listings: find replies that cannot be taken as they are") {
  auto xrd = instance();
  xrd->file("/eos/d/a", "a");
  xrd->file("/eos/d/b", "b");
  const std::string self =
      R"(path="/eos/d/" type=directory  size=0 uid=0 gid=0 mode=40755 mtime=1.0)";
  auto file_line = [](const std::string& name) {
    return R"(path="/eos/d/)" + name + R"(" type=file  size=1 uid=0 gid=0 flags=644 mtime=1.0)";
  };
  std::string answer;
  xrd->proc = [&](std::string_view query) -> std::optional<std::string> {
    if (!query.starts_with("mgm.cmd.proto=")) return std::nullopt;
    return answer;
  };
  auto names = [](const Listing& listing) {
    std::vector<std::string> out;
    for (const Entry& e : listing.entries) out.push_back(e.name);
    std::sort(out.begin(), out.end());
    return out;
  };
  auto ep = eos(xrd);

  SUBCASE("a line that cannot be parsed is skipped") {
    answer = proc_reply(self + "\nnot a find line\n" + file_line("a") + "\n");
    auto listing = ep->list("");
    REQUIRE(listing.ok());
    CHECK(names(listing.value()) == std::vector<std::string>{"a"});
    CHECK(listing.value().skipped == 1);
  }
  SUBCASE("a name listed twice, forged by a line break, is skipped") {
    answer = proc_reply(self + "\n" + file_line("a") + "\n" + file_line("a") + "\n");
    auto listing = ep->list("");
    REQUIRE(listing.ok());
    CHECK(listing.value().entries.empty());
    CHECK(listing.value().skipped == 2);
  }
  SUBCASE("a listing that does not name the directory itself is refused") {
    answer = proc_reply(file_line("a") + "\n");
    auto listing = ep->list("");
    REQUIRE_FALSE(listing.ok());
    CHECK(listing.error().kind == ErrorKind::Other);
  }
  SUBCASE("a listing cut short is completed from a plain listing") {
    // "x" does not exist: its line was forged. "b" was cut off.
    answer = proc_reply(self + "\n" + file_line("a") + "\n" + file_line("x") + "\n", "", E2BIG);
    auto listing = ep->list("");
    REQUIRE(listing.ok());
    CHECK(names(listing.value()) == std::vector<std::string>{"a", "b"});
    CHECK(logged(xrd, "dirlist /eos/d"));
  }
  SUBCASE("subdirectories left out for lack of permission are listed all the same") {
    xrd->dir("/eos/d/secret", 0700);
    answer = proc_reply(self + "\n" + file_line("a") + "\n",
                        "error(13): no permissions to read directory /eos/d/secret/\n", EACCES);
    auto listing = ep->list("");
    REQUIRE(listing.ok());
    CHECK(names(listing.value()) == std::vector<std::string>{"a", "secret"});
  }
  SUBCASE("other errors fail the listing") {
    answer = proc_reply("", "error: something else", EIO);
    CHECK_FALSE(ep->list("").ok());
    answer = proc_reply("", "error(13): permission denied for /eos/d/\n", EACCES);
    CHECK_FALSE(ep->list("").ok());
  }
}
