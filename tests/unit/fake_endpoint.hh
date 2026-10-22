// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "eosmirror/atomic_upload.hh"
#include "eosmirror/endpoint.hh"

// An in-memory endpoint for engine tests, with fault injection.
//
// Every operation first calls `hook` (if set) and then consults the fault
// list: a matching fault makes the operation fail with its error and is used
// up. Operation counts are kept per operation name.
//
// Creating or removing entries bumps the parent directory's mtime, as real
// file systems do, so that tests can check that directory mtimes are fixed
// after their entries. Writes verify the checksum when `caps.checksum` is set,
// like EOS does. With `atomic_uploads`, they are confirmed afterwards as on
// EOS, by the id that each upload gives its file and a "confirm" query. A
// "close" fault fails a commit after the file is in place. A "lost" fault,
// like a close whose connection is gone, makes it succeed without the file.
// Without `caps.can_set_mode`, setting a mode fails.
class FakeEndpoint : public eosmirror::Endpoint {
 public:
  using Entry = eosmirror::Entry;
  using RelPath = eosmirror::RelPath;
  using Error = eosmirror::Error;
  using ErrorKind = eosmirror::ErrorKind;
  using Status = eosmirror::Status;
  template <class T>
  using Result = eosmirror::Result<T>;

  struct Node {
    Entry entry;
    std::string content;
  };

  struct Fault {
    std::string op;
    RelPath path;
    Error error;
    int remaining;
  };

  explicit FakeEndpoint(std::string name = "fake") : name_(std::move(name)) {
    Entry root;
    root.type = eosmirror::EntryType::Directory;
    root.mode = 0755;
    root.mtime = {1000, 0};
    nodes_[""] = Node{root, {}};
    caps.can_set_owner = true;
  }

  eosmirror::Capabilities caps;
  std::function<void(std::string_view op, const RelPath& path)> hook;
  bool utimes_needs_write = false;  // like EOS: utimes requires write access
  // Like EOS: readers take the listed entry as the file's state if the file
  // has the listed size at the open, and detect changes during the read by
  // the size and mtime it had at the open, without a stat.
  bool readers_take_listing = false;
  // Like a user other than root: creating and removing entries requires
  // the owner's write bit on the directory.
  bool entries_need_write = false;
  // Like EOS: files are written in place and confirmed by AtomicUploadWriter,
  // against the checksum of `caps.checksum` where set.
  bool atomic_uploads = false;
  // What runs with this endpoint as target print about it at the start.
  std::string warning, note;
  // Entries that listings leave out as unsafe and count as skipped, like
  // lines of an EOS listing that cannot be parsed.
  std::set<RelPath> unlistable;
  // Entries that listings leave out without saying so, like a plain XRootD
  // server's listing that ends at an entry it cannot stat.
  std::set<RelPath> unlisted;
  // Directories whose listings say that they replace files at the open, like
  // EOS directories that force uploads to be non-atomic.
  std::set<RelPath> replacing_at_open;
  // Listings carry file checksums of type `caps.checksum`, like EOS listings.
  bool lists_checksums = false;
  // With `atomic_uploads`: this many commits leave the upload under its
  // hidden name (".tmp-<name>"), like an EOS close that timed out while the
  // storage node was still committing.
  int hidden_uploads = 0;
  // With `atomic_uploads`: this many aborts fail to discard the upload,
  // which is committed as it is, like an EOS upload whose request to delete
  // failed before its close.
  int committing_aborts = 0;

  // ---- test setup -----------------------------------------------------------

  void fail(std::string op, RelPath path, Error error, int times = 1) {
    std::lock_guard lock(mutex_);
    faults_.push_back({std::move(op), std::move(path), std::move(error), times});
  }

  void clear_faults() {
    std::lock_guard lock(mutex_);
    faults_.clear();
  }

  int count(const std::string& op) const {
    std::lock_guard lock(mutex_);
    auto it = counts_.find(op);
    return it == counts_.end() ? 0 : it->second;
  }

  Entry& add_dir(const RelPath& path, eosmirror::ModeBits mode = 0755,
                 eosmirror::Timespec mtime = {2000, 0}) {
    Entry e = base(path, eosmirror::EntryType::Directory, mode, mtime);
    return put(path, Node{e, {}});
  }

  Entry& add_file(const RelPath& path, std::string content, eosmirror::ModeBits mode = 0644,
                  eosmirror::Timespec mtime = {3000, 0}) {
    Entry e = base(path, eosmirror::EntryType::File, mode, mtime);
    e.size = content.size();
    return put(path, Node{e, std::move(content)});
  }

  Entry& add_symlink(const RelPath& path, std::string target,
                     eosmirror::Timespec mtime = {4000, 0}) {
    Entry e = base(path, eosmirror::EntryType::Symlink, 0777, mtime);
    e.link_target = std::move(target);
    return put(path, Node{e, {}});
  }

  Entry& add_special(const RelPath& path) {
    Entry e = base(path, eosmirror::EntryType::Other, 0600, {5000, 0});
    return put(path, Node{e, {}});
  }

  std::optional<Node> get(const RelPath& path) const {
    std::lock_guard lock(mutex_);
    auto it = nodes_.find(path);
    if (it == nodes_.end()) return std::nullopt;
    return it->second;
  }

  // Removes an entry and everything below it, e.g. from a hook.
  void erase(const RelPath& path) {
    std::lock_guard lock(mutex_);
    std::erase_if(nodes_, [&](const auto& n) {
      return n.first == path || n.first.starts_with(path + "/");
    });
  }

  // Changes an entry in place, e.g. from a hook.
  void modify(const RelPath& path, const std::function<void(Node&)>& f) {
    std::lock_guard lock(mutex_);
    auto it = nodes_.find(path);
    if (it != nodes_.end()) f(it->second);
  }

  std::vector<RelPath> paths() const {
    std::lock_guard lock(mutex_);
    std::vector<RelPath> result;
    for (const auto& [p, n] : nodes_)
      if (!p.empty()) result.push_back(p);
    return result;
  }

  // ---- Endpoint --------------------------------------------------------------

  std::string describe() const override { return name_; }
  eosmirror::Location location() const override { return {name_, "/"}; }
  eosmirror::Capabilities capabilities() const override {
    eosmirror::Capabilities c = caps;
    c.confirms_after_commit = atomic_uploads;
    return c;
  }
  bool is_temporary(const Entry& e) const override {
    return e.type == eosmirror::EntryType::File && e.name.starts_with(".tmp-");
  }
  // Directories with the id "other-fs" are on another file system.
  bool other_file_system(const Entry& e) const override { return e.id == "other-fs"; }
  std::string target_warning() const override { return warning; }
  std::string target_note() const override { return note; }

  Result<Entry> stat(const RelPath& path) override {
    if (auto err = check("stat", path)) return *err;
    std::lock_guard lock(mutex_);
    auto it = nodes_.find(path);
    if (it != nodes_.end()) return it->second.entry;
    // Below an entry that is no directory, as a file system answers.
    for (RelPath above = parent_of(path); !above.empty(); above = parent_of(above)) {
      auto a = nodes_.find(above);
      if (a == nodes_.end()) continue;
      if (a->second.entry.type != eosmirror::EntryType::Directory)
        return Error{ErrorKind::NotADirectory, above + " is no directory"};
      break;
    }
    return not_found(path);
  }

  Result<eosmirror::Listing> list(const RelPath& dir) override {
    if (auto err = check("list", dir)) return *err;
    std::lock_guard lock(mutex_);
    auto it = nodes_.find(dir);
    if (it == nodes_.end()) return not_found(dir);
    if (it->second.entry.type != eosmirror::EntryType::Directory)
      return Error{ErrorKind::NotADirectory, "list " + dir};
    eosmirror::Listing result;
    result.replaces_at_open = replacing_at_open.count(dir) > 0;
    for (const auto& [p, n] : nodes_) {
      if (p.empty() || parent_of(p) != dir || unlisted.count(p)) continue;
      if (unlistable.count(p)) {
        ++result.skipped;
      } else {
        result.entries.push_back(n.entry);
        if (lists_checksums && n.entry.type == eosmirror::EntryType::File)
          result.entries.back().checksum = checksum_of(n.content);
      }
    }
    return result;
  }

  Status mkdir(const RelPath& path, eosmirror::ModeBits mode) override {
    if (auto err = check("mkdir", path)) return *err;
    std::lock_guard lock(mutex_);
    if (nodes_.count(path)) return Error{ErrorKind::Exists, "mkdir " + path};
    if (Status s = require_parent(path, true); !s.ok()) return s;
    // Like the real endpoints: created with the owner's rwx bits added.
    Entry e = base(path, eosmirror::EntryType::Directory, mode | 0700, tick());
    nodes_[path] = Node{e, {}};
    touch_parent(path);
    return {};
  }

  Status symlink(const RelPath& path, const std::string& target) override {
    if (auto err = check("symlink", path)) return *err;
    std::lock_guard lock(mutex_);
    if (Status s = require_parent(path, true); !s.ok()) return s;
    auto it = nodes_.find(path);
    if (it != nodes_.end() && it->second.entry.type == eosmirror::EntryType::Directory)
      return Error{ErrorKind::IsADirectory, "symlink " + path};
    Entry e = base(path, eosmirror::EntryType::Symlink, 0777, tick());
    e.link_target = target;
    nodes_[path] = Node{e, {}};
    touch_parent(path);
    return {};
  }

  Status set_metadata(const RelPath& path, const Entry& md, eosmirror::MetaFields fields) override {
    if (auto err = check("set_metadata", path)) return *err;
    std::lock_guard lock(mutex_);
    auto it = nodes_.find(path);
    if (it == nodes_.end()) return not_found(path);
    if (has(fields, eosmirror::MetaFields::Mode) && !caps.can_set_mode)
      return Error{ErrorKind::Unsupported, "chmod " + path};
    if (utimes_needs_write && has(fields, eosmirror::MetaFields::Mtime) &&
        !(it->second.entry.mode & 0200))
      return Error{ErrorKind::Permission, "utimes " + path + ": no write access"};
    apply(it->second.entry, md, fields);
    return {};
  }

  Status remove(const RelPath& path, eosmirror::EntryType type) override {
    if (auto err = check("remove", path)) return *err;
    std::lock_guard lock(mutex_);
    auto it = nodes_.find(path);
    if (it == nodes_.end()) return not_found(path);
    if (Status s = require_parent(path, true); !s.ok()) return s;
    if (type == eosmirror::EntryType::Directory) {
      for (const auto& [p, n] : nodes_)
        if (!p.empty() && parent_of(p) == path) return Error{ErrorKind::NotEmpty, "remove " + path};
    }
    nodes_.erase(it);
    touch_parent(path);
    return {};
  }

  Status flush() override {
    if (auto err = check("flush", "")) return *err;
    return {};
  }

  Result<std::unique_ptr<eosmirror::FileReader>> open_read(const RelPath& path) override {
    if (auto err = check("open_read", path)) return *err;
    std::lock_guard lock(mutex_);
    auto it = nodes_.find(path);
    if (it == nodes_.end()) return not_found(path);
    if (it->second.entry.type == eosmirror::EntryType::Directory)
      return Error{ErrorKind::IsADirectory, "open " + path};
    if (it->second.entry.type != eosmirror::EntryType::File)
      return Error{ErrorKind::Other, "open " + path + ": not a regular file"};
    return std::unique_ptr<eosmirror::FileReader>(new Reader(*this, path, it->second.entry));
  }

  Result<std::unique_ptr<eosmirror::FileWriter>> open_write(
      const RelPath& path, const eosmirror::CommitSpec& spec) override {
    if (auto err = check("open_write", path)) return *err;
    {
      std::lock_guard lock(mutex_);
      if (Status s = require_parent(path, true); !s.ok()) return s.error();
    }
    std::string id;
    if (atomic_uploads) {
      std::lock_guard lock(mutex_);
      id = "upload" + std::to_string(++uploads_);
    }
    std::unique_ptr<eosmirror::FileWriter> writer(new Writer(*this, path, spec, id));
    if (!atomic_uploads) return writer;
    return std::unique_ptr<eosmirror::FileWriter>(new eosmirror::AtomicUploadWriter(
        *this, path, path, std::move(writer), [this, path, id] { return placed(path, id); }, true,
        spec));
  }

 private:
  class Reader : public eosmirror::FileReader {
   public:
    Reader(FakeEndpoint& ep, RelPath path, Entry at_open)
        : ep_(ep), path_(std::move(path)), at_open_(std::move(at_open)) {}
    Result<size_t> read(uint64_t offset, std::span<std::byte> buf) override {
      if (auto err = ep_.check("read", path_)) return *err;
      std::lock_guard lock(ep_.mutex_);
      auto it = ep_.nodes_.find(path_);
      if (it == ep_.nodes_.end()) return ep_.not_found(path_);
      const std::string& c = it->second.content;
      if (offset >= c.size()) return size_t{0};
      size_t n = std::min(buf.size(), c.size() - static_cast<size_t>(offset));
      std::copy_n(reinterpret_cast<const std::byte*>(c.data() + offset), n, buf.begin());
      return n;
    }
    Result<Entry> stat() override { return ep_.stat(path_); }

    Result<Entry> state(const Entry& listed) override {
      took_listing_ = ep_.readers_take_listing && at_open_.size == listed.size;
      if (took_listing_) return listed;
      return FileReader::state(listed);
    }

    Result<bool> unchanged(const Entry& state) override {
      if (!took_listing_) return FileReader::unchanged(state);
      std::lock_guard lock(ep_.mutex_);
      auto it = ep_.nodes_.find(path_);
      if (it == ep_.nodes_.end()) return ep_.not_found(path_);
      return it->second.entry.size == at_open_.size && it->second.entry.mtime == at_open_.mtime;
    }

   private:
    FakeEndpoint& ep_;
    RelPath path_;
    Entry at_open_;
    bool took_listing_ = false;
  };

  class Writer : public eosmirror::FileWriter {
   public:
    Writer(FakeEndpoint& ep, RelPath path, eosmirror::CommitSpec spec, std::string id)
        : ep_(ep), path_(std::move(path)), spec_(std::move(spec)), id_(std::move(id)) {}
    Status write(eosmirror::Chunk chunk) override {
      if (auto err = ep_.check("write", path_)) return *err;
      if (chunk.offset != content_.size()) return Error{ErrorKind::Other, "non-sequential write"};
      content_.append(reinterpret_cast<const char*>(chunk.data().data()), chunk.size);
      return {};
    }
    Result<eosmirror::CommitInfo> commit(const eosmirror::CommitSpec& spec) override {
      if (auto err = ep_.check("commit", path_)) return *err;
      if (ep_.check("lost", path_)) return eosmirror::CommitInfo{};
      if (content_.size() != spec.size) return Error{ErrorKind::Changed, "size mismatch"};
      if (has(spec.fields, eosmirror::MetaFields::Mode) && !ep_.caps.can_set_mode)
        return Error{ErrorKind::Unsupported, "chmod " + path_};
      bool verified = false;
      if (ep_.caps.checksum != eosmirror::ChecksumType::None &&
          spec.checksum.type == ep_.caps.checksum) {
        eosmirror::Hasher h(spec.checksum.type);
        h.update({reinterpret_cast<const std::byte*>(content_.data()), content_.size()});
        if (!(h.finish() == spec.checksum))
          return Error{ErrorKind::Checksum, "checksum mismatch on " + path_};
        verified = true;
      } else if (spec.require_verification) {
        return Error{ErrorKind::Unsupported, "no checksum on " + path_};
      }
      {
        std::lock_guard lock(ep_.mutex_);
        if (Status s = ep_.require_parent(path_, true); !s.ok()) return s.error();
        auto it = ep_.nodes_.find(path_);
        if (it != ep_.nodes_.end() && it->second.entry.type == eosmirror::EntryType::Directory)
          return Error{ErrorKind::IsADirectory, "commit " + path_};
        RelPath at = path_;
        if (!id_.empty() && ep_.hidden_uploads > 0) {
          --ep_.hidden_uploads;
          at = eosmirror::join(parent_of(path_), ".tmp-" + name_of(path_));
        }
        Entry e = ep_.base(at, eosmirror::EntryType::File, 0600, ep_.tick());
        e.size = content_.size();
        e.id = id_;
        ep_.apply(e, spec.metadata, spec.fields);
        ep_.nodes_[at] = Node{e, content_};
        ep_.touch_parent(at);
      }
      if (auto err = ep_.check("close", path_)) return *err;
      return eosmirror::CommitInfo{verified};
    }
    Status abort() override {
      std::lock_guard lock(ep_.mutex_);
      if (id_.empty() || ep_.committing_aborts <= 0) return {};
      --ep_.committing_aborts;
      Entry e = ep_.base(path_, eosmirror::EntryType::File, 0600, ep_.tick());
      e.size = content_.size();
      e.id = id_;
      ep_.apply(e, spec_.metadata, spec_.fields);
      ep_.nodes_[path_] = Node{e, content_};
      ep_.touch_parent(path_);
      return Error{ErrorKind::IO, "cannot discard the upload of " + path_};
    }

   private:
    FakeEndpoint& ep_;
    RelPath path_;
    eosmirror::CommitSpec spec_;
    std::string id_;
    std::string content_;
  };

  static RelPath parent_of(const RelPath& path) {
    auto slash = path.rfind('/');
    return slash == std::string::npos ? RelPath() : path.substr(0, slash);
  }

  static std::string name_of(const RelPath& path) {
    auto slash = path.rfind('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
  }

  static Error not_found(const RelPath& path) { return Error{ErrorKind::NotFound, "no " + path}; }

  // The file of an upload as it is now, in place or wherever it was moved,
  // with the checksum of `caps.checksum`.
  Result<eosmirror::UploadFile> placed(const RelPath& path, const std::string& id) {
    if (auto err = check("confirm", path)) return *err;
    std::lock_guard lock(mutex_);
    for (const auto& [p, n] : nodes_) {
      if (n.entry.id != id) continue;
      eosmirror::UploadFile found;
      found.file = n.entry;
      found.file.checksum = checksum_of(n.content);
      found.in_place = p == path;
      if (!found.in_place && parent_of(p) != parent_of(path)) found.file.name.clear();
      return found;
    }
    return not_found(path);
  }

  // The checksum of type `caps.checksum`, or none if that is unset.
  eosmirror::Checksum checksum_of(const std::string& content) const {
    if (caps.checksum == eosmirror::ChecksumType::None) return {};
    eosmirror::Hasher h(caps.checksum);
    h.update({reinterpret_cast<const std::byte*>(content.data()), content.size()});
    return h.finish();
  }

  Entry base(const RelPath& path, eosmirror::EntryType type, eosmirror::ModeBits mode,
             eosmirror::Timespec mtime) const {
    Entry e;
    e.name = name_of(path);
    e.type = type;
    e.mode = mode;
    e.mtime = mtime;
    e.uid = 1000;
    e.gid = 1000;
    return e;
  }

  Entry& put(const RelPath& path, Node node) {
    std::lock_guard lock(mutex_);
    return (nodes_[path] = std::move(node)).entry;
  }

  // Requires mutex_.
  Status require_parent(const RelPath& path, bool writing = false) const {
    auto it = nodes_.find(parent_of(path));
    if (it == nodes_.end()) return not_found(parent_of(path));
    if (it->second.entry.type != eosmirror::EntryType::Directory)
      return Error{ErrorKind::NotADirectory, parent_of(path)};
    if (writing && entries_need_write && !(it->second.entry.mode & 0200))
      return Error{ErrorKind::Permission, "no write access to " + parent_of(path)};
    return {};
  }

  // Requires mutex_. Entries created by the endpoint get increasing mtimes.
  eosmirror::Timespec tick() { return {++clock_, 0}; }

  // Requires mutex_.
  void touch_parent(const RelPath& path) {
    auto it = nodes_.find(parent_of(path));
    if (it != nodes_.end()) it->second.entry.mtime = tick();
  }

  static void apply(Entry& e, const Entry& md, eosmirror::MetaFields fields) {
    if (has(fields, eosmirror::MetaFields::Owner)) {
      e.uid = md.uid;
      e.gid = md.gid;
    }
    if (has(fields, eosmirror::MetaFields::Mode) && e.type != eosmirror::EntryType::Symlink)
      e.mode = md.mode;
    if (has(fields, eosmirror::MetaFields::Mtime)) e.mtime = md.mtime;
  }

  std::optional<Error> check(std::string_view op, const RelPath& path) {
    if (hook) hook(op, path);
    std::lock_guard lock(mutex_);
    ++counts_[std::string(op)];
    for (auto it = faults_.begin(); it != faults_.end(); ++it) {
      if (it->op == op && it->path == path) {
        Error e = it->error;
        if (--it->remaining <= 0) faults_.erase(it);
        return e;
      }
    }
    return std::nullopt;
  }

  std::string name_;
  mutable std::mutex mutex_;
  std::map<RelPath, Node> nodes_;
  std::vector<Fault> faults_;
  std::map<std::string, int> counts_;
  int64_t clock_ = 10000;
  int uploads_ = 0;
};
