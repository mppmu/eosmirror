// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

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
// like EOS does.
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

  // ---- test setup -----------------------------------------------------------

  void fail(std::string op, RelPath path, Error error, int times = 1) {
    std::lock_guard lock(mutex_);
    faults_.push_back({std::move(op), std::move(path), std::move(error), times});
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
  eosmirror::Capabilities capabilities() const override { return caps; }

  Result<Entry> stat(const RelPath& path) override {
    if (auto err = check("stat", path)) return *err;
    std::lock_guard lock(mutex_);
    auto it = nodes_.find(path);
    if (it == nodes_.end()) return not_found(path);
    return it->second.entry;
  }

  Result<std::vector<Entry>> list(const RelPath& dir) override {
    if (auto err = check("list", dir)) return *err;
    std::lock_guard lock(mutex_);
    auto it = nodes_.find(dir);
    if (it == nodes_.end()) return not_found(dir);
    if (it->second.entry.type != eosmirror::EntryType::Directory)
      return Error{ErrorKind::NotADirectory, "list " + dir};
    std::vector<Entry> result;
    for (const auto& [p, n] : nodes_)
      if (!p.empty() && parent_of(p) == dir) result.push_back(n.entry);
    return result;
  }

  Status mkdir(const RelPath& path, eosmirror::ModeBits mode) override {
    if (auto err = check("mkdir", path)) return *err;
    std::lock_guard lock(mutex_);
    if (nodes_.count(path)) return Error{ErrorKind::Exists, "mkdir " + path};
    if (Status s = require_parent(path); !s.ok()) return s;
    Entry e = base(path, eosmirror::EntryType::Directory, mode, tick());
    nodes_[path] = Node{e, {}};
    touch_parent(path);
    return {};
  }

  Status symlink(const RelPath& path, const std::string& target) override {
    if (auto err = check("symlink", path)) return *err;
    std::lock_guard lock(mutex_);
    if (Status s = require_parent(path); !s.ok()) return s;
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
    apply(it->second.entry, md, fields);
    return {};
  }

  Status remove(const RelPath& path, eosmirror::EntryType type) override {
    if (auto err = check("remove", path)) return *err;
    std::lock_guard lock(mutex_);
    auto it = nodes_.find(path);
    if (it == nodes_.end()) return not_found(path);
    if (type == eosmirror::EntryType::Directory) {
      for (const auto& [p, n] : nodes_)
        if (!p.empty() && parent_of(p) == path) return Error{ErrorKind::NotEmpty, "remove " + path};
    }
    nodes_.erase(it);
    touch_parent(path);
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
    return std::unique_ptr<eosmirror::FileReader>(new Reader(*this, path));
  }

  Result<std::unique_ptr<eosmirror::FileWriter>> open_write(
      const RelPath& path, const eosmirror::CommitSpec& spec) override {
    if (auto err = check("open_write", path)) return *err;
    std::lock_guard lock(mutex_);
    if (Status s = require_parent(path); !s.ok()) return s.error();
    return std::unique_ptr<eosmirror::FileWriter>(new Writer(*this, path, spec));
  }

 private:
  class Reader : public eosmirror::FileReader {
   public:
    Reader(FakeEndpoint& ep, RelPath path) : ep_(ep), path_(std::move(path)) {}
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

   private:
    FakeEndpoint& ep_;
    RelPath path_;
  };

  class Writer : public eosmirror::FileWriter {
   public:
    Writer(FakeEndpoint& ep, RelPath path, eosmirror::CommitSpec spec)
        : ep_(ep), path_(std::move(path)), spec_(std::move(spec)) {}
    Status write(uint64_t offset, std::span<const std::byte> data) override {
      if (auto err = ep_.check("write", path_)) return *err;
      if (offset != content_.size()) return Error{ErrorKind::Other, "non-sequential write"};
      content_.append(reinterpret_cast<const char*>(data.data()), data.size());
      return {};
    }
    Result<eosmirror::CommitInfo> commit(const eosmirror::CommitSpec& spec) override {
      if (auto err = ep_.check("commit", path_)) return *err;
      if (content_.size() != spec.size) return Error{ErrorKind::Changed, "size mismatch"};
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
      std::lock_guard lock(ep_.mutex_);
      if (Status s = ep_.require_parent(path_); !s.ok()) return s.error();
      auto it = ep_.nodes_.find(path_);
      if (it != ep_.nodes_.end() && it->second.entry.type == eosmirror::EntryType::Directory)
        return Error{ErrorKind::IsADirectory, "commit " + path_};
      Entry e = ep_.base(path_, eosmirror::EntryType::File, 0600, ep_.tick());
      e.size = content_.size();
      ep_.apply(e, spec.metadata, spec.fields);
      ep_.nodes_[path_] = Node{e, content_};
      ep_.touch_parent(path_);
      return eosmirror::CommitInfo{verified};
    }
    void abort() override {}

   private:
    FakeEndpoint& ep_;
    RelPath path_;
    eosmirror::CommitSpec spec_;
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
  Status require_parent(const RelPath& path) const {
    auto it = nodes_.find(parent_of(path));
    if (it == nodes_.end()) return not_found(parent_of(path));
    if (it->second.entry.type != eosmirror::EntryType::Directory)
      return Error{ErrorKind::NotADirectory, parent_of(path)};
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
};
