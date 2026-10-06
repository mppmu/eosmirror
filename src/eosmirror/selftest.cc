// SPDX-License-Identifier: GPL-3.0-or-later
#include "eosmirror/selftest.hh"

#include <algorithm>
#include <cstdio>
#include <span>

#include "eosmirror/checksum.hh"

namespace eosmirror {

namespace {

std::span<const std::byte> bytes(std::string_view s) {
  return {reinterpret_cast<const std::byte*>(s.data()), s.size()};
}

class Selftest {
 public:
  Selftest(Endpoint& target, bool with_owner)
      : target_(target), caps_(target.capabilities()), with_owner_(with_owner) {}

  SelftestReport run() {
    dir_ = ".eosmirror-selftest-" + random_suffix();
    if (!check("create a directory", target_.mkdir(dir_, 0750))) return report_;
    write_file();
    list_directory();
    replace_file();
    symlink();
    directory_metadata();
    cleanup();
    return report_;
  }

 private:
  bool check(const std::string& name, const Status& status) {
    SelftestCheck c{name, status.ok(), false, status.ok() ? "" : status.error().describe()};
    report_.checks.push_back(c);
    return c.ok;
  }

  void pass(const std::string& name, std::string message = "") {
    report_.checks.push_back({name, true, false, std::move(message)});
  }

  void fail(const std::string& name, std::string message) {
    report_.checks.push_back({name, false, false, std::move(message)});
  }

  void skip(const std::string& name, std::string message) {
    report_.checks.push_back({name, true, true, std::move(message)});
  }

  Entry wanted_metadata(EntryType type) const {
    Entry md;
    md.type = type;
    md.mode = type == EntryType::Directory ? 02750 : 0640;
    md.uid = 12345;
    md.gid = 23456;
    md.mtime = {1500000000, caps_.mtime_resolution >= 1000000000 ? 0 : 123456789};
    return md;
  }

  MetaFields wanted_fields(EntryType type) const {
    MetaFields fields = MetaFields::None;
    if (caps_.can_set_mtime) fields = fields | MetaFields::Mtime;
    if (caps_.can_set_mode && type != EntryType::Symlink) fields = fields | MetaFields::Mode;
    if (with_owner_ && caps_.can_set_owner && (type != EntryType::Symlink || caps_.symlink_owner))
      fields = fields | MetaFields::Owner;
    return fields;
  }

  // Compares an entry with the metadata that was set.
  void check_metadata(const std::string& name, const Entry& e, const Entry& md,
                      MetaFields fields) {
    std::string problems;
    char buf[80];
    if (has(fields, MetaFields::Mtime) &&
        e.mtime.truncated(caps_.mtime_resolution) != md.mtime.truncated(caps_.mtime_resolution)) {
      std::snprintf(buf, sizeof buf, " mtime %lld.%09d instead of %lld.%09d",
                    static_cast<long long>(e.mtime.sec), e.mtime.nsec,
                    static_cast<long long>(md.mtime.sec), md.mtime.nsec);
      problems += buf;
    }
    ModeBits bits = md.type == EntryType::Directory ? caps_.dir_mode_bits : caps_.file_mode_bits;
    if (has(fields, MetaFields::Mode) && (e.mode & bits) != (md.mode & bits)) {
      std::snprintf(buf, sizeof buf, " mode %04o instead of %04o", e.mode, md.mode & bits);
      problems += buf;
    }
    if (has(fields, MetaFields::Owner) && (e.uid != md.uid || e.gid != md.gid)) {
      std::snprintf(buf, sizeof buf, " owner %u:%u instead of %u:%u", e.uid, e.gid, md.uid, md.gid);
      problems += buf;
    }
    if (problems.empty())
      pass(name);
    else
      fail(name, "not as set:" + problems);
  }

  void write_file() {
    std::string content(300000, '\0');
    for (size_t i = 0; i < content.size(); ++i) content[i] = static_cast<char>('a' + (i * 7) % 26);
    Hasher hasher(caps_.checksum != ChecksumType::None ? caps_.checksum : ChecksumType::Adler32);
    hasher.update(bytes(content));
    file_ = join(dir_, "file");
    CommitSpec spec;
    spec.size = content.size();
    spec.metadata = wanted_metadata(EntryType::File);
    spec.fields = wanted_fields(EntryType::File);
    spec.checksum = hasher.finish();
    auto writer = target_.open_write(file_, spec);
    if (!check("create a file", writer.ok() ? Status() : Status(writer.error()))) return;
    for (size_t off = 0; off < content.size(); off += 100000) {
      size_t n = std::min<size_t>(100000, content.size() - off);
      Status w = writer.value()->write(off, bytes(std::string_view(content).substr(off, n)));
      if (!check("write a file", w)) return;
    }
    auto committed = writer.value()->commit(spec);
    if (!check("commit a file", committed.ok() ? Status() : Status(committed.error()))) return;
    file_exists_ = true;
    if (committed.value().verified)
      pass("verify the stored checksum", std::string(to_string(spec.checksum.type)));
    else
      skip("verify the stored checksum", "the target computes no checksum to compare with");

    auto st = target_.stat(file_);
    if (!check("stat the file", st.ok() ? Status() : Status(st.error()))) return;
    if (st.value().size != content.size())
      fail("size of the file", "stored " + std::to_string(st.value().size) + " bytes instead of " +
                                   std::to_string(content.size()));
    else
      pass("size of the file");
    check_metadata("metadata of a new file", st.value(), spec.metadata, spec.fields);
    if (!with_owner_)
      skip("owner of a new file", "owners not requested");
    else if (!caps_.can_set_owner)
      skip("owner of a new file", "the target does not let this identity set owners");
  }

  void list_directory() {
    auto listed = target_.list(dir_);
    if (!check("list the directory", listed.ok() ? Status() : Status(listed.error()))) return;
    bool found = std::any_of(listed.value().begin(), listed.value().end(), [&](const Entry& e) {
      return e.name == "file" && e.type == EntryType::File;
    });
    if (found)
      pass("find the file in the listing");
    else
      fail("find the file in the listing", std::to_string(listed.value().size()) + " entries, none is the file");
  }

  void replace_file() {
    if (!file_exists_) return;
    std::string content = "replaced";
    Hasher hasher(caps_.checksum != ChecksumType::None ? caps_.checksum : ChecksumType::Adler32);
    hasher.update(bytes(content));
    CommitSpec spec;
    spec.size = content.size();
    spec.metadata = wanted_metadata(EntryType::File);
    spec.fields = wanted_fields(EntryType::File);
    spec.checksum = hasher.finish();
    auto writer = target_.open_write(file_, spec);
    if (!check("replace a file", writer.ok() ? Status() : Status(writer.error()))) return;
    Status w = writer.value()->write(0, bytes(content));
    auto committed = w.ok() ? writer.value()->commit(spec) : Result<CommitInfo>(w.error());
    if (!check("replace a file", committed.ok() ? Status() : Status(committed.error()))) return;
    auto st = target_.stat(file_);
    if (st.ok() && st.value().size == content.size())
      pass("size after replacing");
    else
      fail("size after replacing", st.ok() ? "old size kept" : st.error().describe());
  }

  void symlink() {
    if (!caps_.has_symlinks) {
      skip("create a symlink", "the target has no symlinks");
      return;
    }
    link_ = join(dir_, "link");
    if (!check("create a symlink", target_.symlink(link_, "file"))) return;
    link_exists_ = true;
    auto st = target_.stat(link_);
    if (!check("stat the symlink", st.ok() ? Status() : Status(st.error()))) return;
    if (st.value().type == EntryType::Symlink && st.value().link_target == "file")
      pass("symlink target");
    else
      fail("symlink target", "stored '" + st.value().link_target + "'");
    Entry md = wanted_metadata(EntryType::Symlink);
    MetaFields fields = wanted_fields(EntryType::Symlink);
    if (fields == MetaFields::None) return;
    if (!check("set symlink metadata", target_.set_metadata(link_, md, fields))) return;
    st = target_.stat(link_);
    if (st.ok()) check_metadata("metadata of the symlink", st.value(), md, fields);
  }

  void directory_metadata() {
    Entry md = wanted_metadata(EntryType::Directory);
    MetaFields fields = wanted_fields(EntryType::Directory);
    if (fields == MetaFields::None) return;
    if (!check("set directory metadata", target_.set_metadata(dir_, md, fields))) return;
    auto st = target_.stat(dir_);
    if (st.ok()) check_metadata("metadata of the directory", st.value(), md, fields);
  }

  void cleanup() {
    if (link_exists_) check("remove the symlink", target_.remove(link_, EntryType::Symlink));
    if (file_exists_) check("remove the file", target_.remove(file_, EntryType::File));
    // Failed steps may have left entries behind (such as a server's own
    // temporaries); remove whatever is still there.
    auto left = target_.list(dir_);
    if (left.ok()) {
      for (const Entry& e : left.value()) {
        if (!valid_entry_name(e.name)) continue;
        Status s = target_.remove(join(dir_, e.name), e.type);
        if (!s.ok()) fail("remove leftover " + e.name, s.error().describe());
      }
    }
    check("remove the directory", target_.remove(dir_, EntryType::Directory));
  }

  Endpoint& target_;
  Capabilities caps_;
  bool with_owner_;
  SelftestReport report_;
  RelPath dir_, file_, link_;
  bool file_exists_ = false;
  bool link_exists_ = false;
};

}  // namespace

bool SelftestReport::ok() const {
  return std::all_of(checks.begin(), checks.end(), [](const SelftestCheck& c) { return c.ok; });
}

SelftestReport run_selftest(Endpoint& target, bool with_owner) {
  target.probe_target();
  return Selftest(target, with_owner).run();
}

}  // namespace eosmirror
