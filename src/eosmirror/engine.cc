// SPDX-License-Identifier: GPL-3.0-or-later
#include "eosmirror/engine.hh"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <thread>
#include <unordered_map>
#include <unordered_set>

#include "eosmirror/copy.hh"
#include "eosmirror/log.hh"
#include "eosmirror/queue.hh"
#include "eosmirror/retry.hh"

namespace eosmirror {

namespace {

// A directory being processed. It stays alive until all its entries and
// subdirectories are done, then its metadata is applied.
struct DirNode {
  RelPath path;
  Entry source;
  std::shared_ptr<DirNode> parent;
  bool created = false;  // the target directory did not exist before this run
  Entry target;          // the target's metadata as listed, when not created
  bool owned = true;     // this shard handles the directory's entries
  std::optional<std::set<std::string>> only;  // restrict to these entry names

  std::atomic<int> pending{1};  // own processing plus unfinished entries
  std::atomic<bool> touched{false};        // entries were created, replaced or removed
  std::atomic<bool> failed{false};         // listing, creation or metadata failed
  std::atomic<bool> made_writable{false};  // the target's mode was relaxed for writing
  std::atomic<bool> unverified_warned{false};
  std::mutex writable_mutex;
};

struct CopyJob {
  std::shared_ptr<DirNode> dir;
  Entry source;
};

uint64_t fnv1a(std::string_view s) {
  uint64_t h = 14695981039346656037ULL;
  for (unsigned char c : s) {
    h ^= c;
    h *= 1099511628211ULL;
  }
  return h;
}

RelPath parent_path(const RelPath& path) {
  auto slash = path.rfind('/');
  return slash == std::string::npos ? RelPath() : path.substr(0, slash);
}

std::string name_of(const RelPath& path) {
  auto slash = path.rfind('/');
  return slash == std::string::npos ? path : path.substr(slash + 1);
}

bool is_under(const RelPath& path, const RelPath& dir) {
  if (dir.empty()) return !path.empty();
  return path.size() > dir.size() && path.compare(0, dir.size(), dir) == 0 &&
         path[dir.size()] == '/';
}

}  // namespace

struct Engine::Impl {
  Impl(Endpoint& src, Endpoint& dst, SyncOptions opts, Report& rep, Journal* jnl,
       const Cancellation& cnl)
      : source(src),
        target(dst),
        options(std::move(opts)),
        report(rep),
        stats(rep.stats),
        journal(jnl),
        cancel(cnl),
        dirs(std::numeric_limits<size_t>::max(), /*lifo=*/true),
        copies(std::max<size_t>(1, options.max_backlog)) {
    copy_options.preserve_owner = options.preserve_owner;
    copy_options.preserve_mode = options.preserve_mode;
    copy_options.verify = options.verify || options.require_checksum;
    copy_options.require_verification = options.require_checksum;
  }

  Endpoint& source;
  Endpoint& target;
  SyncOptions options;
  Report& report;
  Stats& stats;
  Journal* journal;
  const Cancellation& cancel;
  CopyOptions copy_options;

  // Settled once the target root exists, since probing it may need that.
  int32_t mtime_resolution = 1;
  bool use_mtimes = true;    // the target stores mtimes: compare and set them
  bool use_symlinks = true;  // the target has symlinks
  bool symlink_owner = true;  // symlinks on the target have settable owners
  bool relax_modes = false;  // make read-only target directories writable first

  WorkQueue<std::shared_ptr<DirNode>> dirs;
  WorkQueue<CopyJob> copies;
  std::vector<std::thread> threads;
  bool ran = false;

  std::mutex done_mutex;
  std::condition_variable done_cv;
  size_t roots_pending = 0;

  std::unordered_set<RelPath> prior_failures;  // paths to clear in the journal on success
  std::atomic<uint64_t> deletions{0};
  std::atomic<bool> delete_cap_reported{false};

  // ---- helpers -------------------------------------------------------------

  template <class Op>
  auto retry(Op&& op) {
    return with_retries(options.retry, cancel, stats.retries, std::forward<Op>(op));
  }

  bool same_mtime(const Timespec& a, const Timespec& b) const {
    return a.truncated(mtime_resolution) == b.truncated(mtime_resolution);
  }

  bool shard_owns(const RelPath& path) const {
    if (options.shard_count <= 1) return true;
    return fnv1a(path) % static_cast<uint64_t>(options.shard_count) ==
           static_cast<uint64_t>(options.shard_index);
  }

  std::string rewrite_link(const std::string& link_target) const {
    for (const auto& [from, to] : options.link_rewrites)
      if (link_target.compare(0, from.size(), from) == 0)
        return to + link_target.substr(from.size());
    return link_target;
  }

  void fail(const RelPath& path, EntryType type, const Error& error) {
    if (error.kind == ErrorKind::Cancelled) return;
    Failure f{path, type, error};
    if (journal && !options.dry_run) journal->record_failure(f);
    report.add_failure(std::move(f));
  }

  // Drops the journal's record of an earlier failure of the path.
  void succeeded(const RelPath& path) {
    if (journal && !options.dry_run && prior_failures.count(path)) journal->clear_failure(path);
  }

  // The metadata fields of the target entry that differ from the source.
  MetaFields differences(const Entry& src, const Entry& dst) const {
    MetaFields fields = MetaFields::None;
    bool owner_matters =
        options.preserve_owner && (src.type != EntryType::Symlink || symlink_owner);
    if (owner_matters && (src.uid != dst.uid || src.gid != dst.gid))
      fields = fields | MetaFields::Owner;
    bool mode_matters = options.preserve_mode && src.type != EntryType::Symlink;
    if (mode_matters && src.mode != dst.mode) fields = fields | MetaFields::Mode;
    // Changing the owner clears setuid and setgid bits, so the mode is
    // reapplied afterwards.
    if (mode_matters && has(fields, MetaFields::Owner) && (src.mode & 06000))
      fields = fields | MetaFields::Mode;
    if (use_mtimes && src.type != EntryType::File && !same_mtime(src.mtime, dst.mtime))
      fields = fields | MetaFields::Mtime;
    return fields;
  }

  // Applies the given fields of the source entry's metadata to the target.
  bool apply_metadata(const RelPath& path, const Entry& src, MetaFields fields) {
    if (fields == MetaFields::None) return true;
    if (options.dry_run) return true;
    Status s = retry([&] { return target.set_metadata(path, src, fields); });
    if (!s.ok()) {
      fail(path, src.type, s.error());
      return false;
    }
    return true;
  }

  // Before changing entries of an existing target directory that may not be
  // writable for us, adds the owner's rwx bits; finalize restores the mode.
  void ensure_writable(const std::shared_ptr<DirNode>& node) {
    if (!relax_modes || node->created || options.dry_run || node->made_writable) return;
    std::lock_guard lock(node->writable_mutex);
    if (node->made_writable) return;
    if ((node->target.mode & 0700) != 0700) {
      Entry relaxed = node->target;
      relaxed.mode |= 0700;
      Status s = retry([&] { return target.set_metadata(node->path, relaxed, MetaFields::Mode); });
      if (!s.ok()) log::warn("cannot make ", node->path, " writable: ", s.error().describe());
    }
    node->made_writable = true;
  }

  // ---- directory completion -------------------------------------------------

  void entry_done(const std::shared_ptr<DirNode>& node) {
    if (node->pending.fetch_sub(1) == 1) finalize(node);
  }

  void finalize(const std::shared_ptr<DirNode>& node) {
    if (!cancel.requested() && node->owned && !node->failed) {
      MetaFields fields = MetaFields::None;
      if (node->created) {
        if (use_mtimes) fields = fields | MetaFields::Mtime;
        if (options.preserve_owner) fields = fields | MetaFields::Owner;
        if (options.preserve_mode) fields = fields | MetaFields::Mode;
      } else {
        fields = differences(node->source, node->target);
        if (node->touched && use_mtimes) fields = fields | MetaFields::Mtime;
        if (node->made_writable && options.preserve_mode) fields = fields | MetaFields::Mode;
        if (fields != MetaFields::None) stats.metadata_fixed.fetch_add(1);
      }
      if (!apply_metadata(node->path, node->source, fields)) node->failed = true;
    }
    if (!node->failed && !cancel.requested()) succeeded(node->path);
    if (journal && !options.dry_run && !node->failed && !node->only && !cancel.requested())
      journal->record_done_dir(node->path);
    if (node->parent) {
      entry_done(node->parent);
    } else {
      std::lock_guard lock(done_mutex);
      --roots_pending;
      done_cv.notify_all();
    }
  }

  // ---- the directory walk -----------------------------------------------------

  void checker_loop() {
    while (auto node = dirs.pop()) process_dir(*node);
  }

  void process_dir(const std::shared_ptr<DirNode>& node) {
    if (cancel.requested()) {
      entry_done(node);
      return;
    }
    if (journal && options.resume && !node->created && !node->only &&
        journal->is_done_dir(node->path)) {
      log::debug("skipping finalized directory ", node->path);
      node->owned = false;  // already finalized, leave its metadata alone
      entry_done(node);
      return;
    }

    auto listed = retry([&] { return source.list(node->path); });
    if (!listed.ok()) {
      node->failed = true;
      fail(node->path, EntryType::Directory, listed.error());
      entry_done(node);
      return;
    }
    std::vector<Entry>& src_entries = listed.value();

    std::unordered_map<std::string, Entry> dst_entries;
    if (!node->created) {
      auto dst_listed = retry([&] { return target.list(node->path); });
      if (dst_listed.ok()) {
        for (Entry& e : dst_listed.value()) {
          std::string name = e.name;
          dst_entries.emplace(std::move(name), std::move(e));
        }
      } else {
        // The directory vanished since the parent was listed: recreate it.
        Status created = dst_listed.error().kind == ErrorKind::NotFound
                             ? create_dir(node)
                             : Status(dst_listed.error());
        if (!created.ok()) {
          node->failed = true;
          fail(node->path, EntryType::Directory, created.error());
          entry_done(node);
          return;
        }
      }
    }
    stats.dirs_listed.fetch_add(1);

    for (Entry& e : src_entries) {
      if (cancel.requested()) break;
      if (node->only && !node->only->count(e.name)) continue;
      auto it = dst_entries.find(e.name);
      const Entry* existing = it == dst_entries.end() ? nullptr : &it->second;
      switch (e.type) {
        case EntryType::Directory: handle_dir(node, e, existing); break;
        case EntryType::File:
          if (node->owned) handle_file(node, e, existing);
          break;
        case EntryType::Symlink:
          if (node->owned) handle_symlink(node, e, existing);
          break;
        case EntryType::Other:
          if (node->owned) {
            stats.specials_skipped.fetch_add(1);
            log::info("skipping special file ", join(node->path, e.name));
          }
          break;
      }
      if (it != dst_entries.end()) dst_entries.erase(it);
    }

    if (node->only) {
      // Names gone from both sides have nothing left to retry.
      for (const std::string& name : *node->only) {
        bool in_source = std::any_of(src_entries.begin(), src_entries.end(),
                                     [&](const Entry& e) { return e.name == name; });
        if (!in_source && !dst_entries.count(name)) succeeded(join(node->path, name));
      }
    } else if (node->owned && !cancel.requested()) {
      handle_extras(node, dst_entries);
    }
    entry_done(node);
  }

  // Creates the target directory of a node whose target does not exist.
  Status create_dir(const std::shared_ptr<DirNode>& node) {
    if (node->parent) {
      ensure_writable(node->parent);
      node->parent->touched = true;
    }
    if (!options.dry_run) {
      Status s = retry([&] { return target.mkdir(node->path, node->source.mode); });
      if (!s.ok()) {
        if (s.error().kind != ErrorKind::Exists) return s;
        // Another shard or a concurrent run got there first.
        auto st = target.stat(node->path);
        if (!st.ok() || st.value().type != EntryType::Directory) return s;
        node->target = st.value();
        return {};
      }
    }
    stats.dirs_created.fetch_add(1);
    node->created = true;
    return {};
  }

  // Removes a target entry that is in the way of a source entry of another
  // type. Returns whether the path is now free.
  bool resolve_conflict(const std::shared_ptr<DirNode>& node, const RelPath& path,
                        const Entry& existing, EntryType wanted) {
    if (!options.delete_extra) {
      fail(path, wanted,
           Error{ErrorKind::Exists, "target is a " + std::string(to_string(existing.type)) +
                                        ", use --delete to replace it"});
      return false;
    }
    ensure_writable(node);
    node->touched = true;
    Status removed = delete_tree(path, existing);
    if (!removed.ok()) {
      Error e = removed.error();
      e.message = "cannot replace the " + std::string(to_string(existing.type)) +
                  " on the target: " + e.message;
      fail(path, wanted, e);
      return false;
    }
    return true;
  }

  void handle_dir(const std::shared_ptr<DirNode>& node, const Entry& e, const Entry* existing) {
    RelPath path = join(node->path, e.name);
    auto child = std::make_shared<DirNode>();
    child->path = path;
    child->source = e;
    child->parent = node;
    child->owned = shard_owns(path);
    if (existing && existing->type != EntryType::Directory) {
      if (!node->owned) {
        log::warn("skipping ", path, " in this run: the target entry is a ",
                  to_string(existing->type), " that another shard replaces");
        return;
      }
      if (!resolve_conflict(node, path, *existing, EntryType::Directory)) return;
      existing = nullptr;
    }
    if (existing) {
      child->target = *existing;
    } else {
      Status created = create_dir(child);
      if (!created.ok()) {
        fail(path, EntryType::Directory, created.error());
        return;
      }
    }
    node->pending.fetch_add(1);
    if (!dirs.push(child)) entry_done(node);
  }

  void handle_file(const std::shared_ptr<DirNode>& node, const Entry& e, const Entry* existing) {
    RelPath path = join(node->path, e.name);
    stats.files_checked.fetch_add(1);
    if (existing && existing->type != EntryType::File) {
      if (!resolve_conflict(node, path, *existing, EntryType::File)) return;
      existing = nullptr;
    }
    if (existing && existing->size == e.size &&
        (!use_mtimes || same_mtime(existing->mtime, e.mtime))) {
      stats.files_unchanged.fetch_add(1);
      MetaFields fields = differences(e, *existing);
      if (!apply_metadata(path, e, fields)) return;
      if (fields != MetaFields::None) stats.metadata_fixed.fetch_add(1);
      succeeded(path);
      return;
    }
    ensure_writable(node);
    node->pending.fetch_add(1);
    if (!copies.push(CopyJob{node, e})) entry_done(node);
  }

  void handle_symlink(const std::shared_ptr<DirNode>& node, const Entry& e,
                      const Entry* existing) {
    RelPath path = join(node->path, e.name);
    if (!use_symlinks) {
      stats.symlinks_skipped.fetch_add(1);
      log::debug("skipping symlink ", path, ": no symlinks on ", target.describe());
      return;
    }
    std::string link_target = rewrite_link(e.link_target);
    if (existing && existing->type != EntryType::Symlink) {
      if (!resolve_conflict(node, path, *existing, EntryType::Symlink)) return;
      existing = nullptr;
    }
    if (existing && existing->link_target == link_target) {
      stats.symlinks_unchanged.fetch_add(1);
      MetaFields fields = differences(e, *existing);
      if (!apply_metadata(path, e, fields)) return;
      if (fields != MetaFields::None) stats.metadata_fixed.fetch_add(1);
      succeeded(path);
      return;
    }
    ensure_writable(node);
    node->touched = true;
    stats.symlinks_created.fetch_add(1);
    if (options.dry_run) return;
    Status s = retry([&] { return target.symlink(path, link_target); });
    if (!s.ok()) {
      fail(path, EntryType::Symlink, s.error());
      return;
    }
    MetaFields fields = MetaFields::None;
    if (use_mtimes) fields = fields | MetaFields::Mtime;
    if (options.preserve_owner && symlink_owner) fields = fields | MetaFields::Owner;
    if (apply_metadata(path, e, fields)) succeeded(path);
  }

  void handle_extras(const std::shared_ptr<DirNode>& node,
                     const std::unordered_map<std::string, Entry>& extras) {
    int64_t now = std::chrono::duration_cast<std::chrono::seconds>(
                      std::chrono::system_clock::now().time_since_epoch())
                      .count();
    for (const auto& [name, e] : extras) {
      if (cancel.requested()) return;
      RelPath path = join(node->path, name);
      if (target.is_temporary(name)) {
        bool stale = e.mtime.sec + options.stale_temp_age.count() < now;
        if (!stale) continue;
        if (!options.delete_extra) {
          stats.stale_temps.fetch_add(1);
          continue;
        }
      } else {
        stats.extras.fetch_add(1);
        if (!options.delete_extra) {
          log::debug("extra entry on target: ", path);
          continue;
        }
      }
      ensure_writable(node);
      node->touched = true;
      Status removed = delete_tree(path, e);
      if (removed.ok()) {
        succeeded(path);
      } else if (!removed.error().message.starts_with("deletion cap") ||
                 !delete_cap_reported.exchange(true)) {
        fail(path, e.type, removed.error());
      }
    }
  }

  // Deletes a target entry, directories recursively, within the deletion
  // cap. Reports nothing; the error of the first entry that could not be
  // removed is returned.
  Status delete_tree(const RelPath& path, const Entry& e) {
    if (e.type == EntryType::Directory) {
      auto listed = retry([&] { return target.list(path); });
      if (!listed.ok()) return listed.error();
      for (const Entry& child : listed.value()) {
        if (cancel.requested()) return Error{ErrorKind::Cancelled, "cancelled"};
        Status s = delete_tree(join(path, child.name), child);
        if (!s.ok()) return s;
      }
    }
    if (deletions.fetch_add(1) >= options.max_delete) {
      deletions.fetch_sub(1);
      return Error{ErrorKind::Other, "deletion cap of " + std::to_string(options.max_delete) +
                                         " reached, not deleting " + path};
    }
    log::info(options.dry_run ? "would delete " : "deleting ", to_string(e.type), " ", path);
    if (!options.dry_run) {
      Status s = retry([&] { return target.remove(path, e.type); });
      if (!s.ok() && s.error().kind != ErrorKind::NotFound) {
        deletions.fetch_sub(1);
        return s;
      }
    }
    stats.deleted.fetch_add(1);
    return {};
  }

  // ---- transfers -----------------------------------------------------------------

  void transfer_loop() {
    std::vector<std::byte> buffer(std::max<size_t>(options.buffer_size, 4096));
    while (auto job = copies.pop()) run_copy(*job, buffer);
  }

  void run_copy(const CopyJob& job, std::span<std::byte> buffer) {
    RelPath path = join(job.dir->path, job.source.name);
    job.dir->touched = true;
    if (options.dry_run) {
      stats.files_copied.fetch_add(1);
      stats.bytes_copied.fetch_add(job.source.size);
      log::debug("would copy ", path);
    } else if (!cancel.requested()) {
      auto result =
          retry([&] { return copy_file(source, target, path, copy_options, buffer, cancel); });
      if (result.ok()) {
        stats.files_copied.fetch_add(1);
        stats.bytes_copied.fetch_add(result.value().bytes);
        log::debug("copied ", path, " (", result.value().bytes, " bytes)");
        if (copy_options.verify && !result.value().verified) {
          stats.files_unverified.fetch_add(1);
          if (!job.dir->unverified_warned.exchange(true))
            log::warn("no checksum verification for files copied into ",
                      job.dir->path.empty() ? "." : job.dir->path);
        }
        succeeded(path);
      } else {
        fail(path, EntryType::File, result.error());
      }
    }
    entry_done(job.dir);
  }

  // ---- running ----------------------------------------------------------------------

  Status preflight() {
    if (ran) return Error{ErrorKind::Other, "an engine runs only once"};
    ran = true;
    if (options.preserve_owner && !target.capabilities().can_set_owner)
      return Error{ErrorKind::Permission,
                   "cannot set owners on " + target.describe() + " (use --no-owner to copy anyway)"};
    if (options.shard_count < 1 || options.shard_index < 0 ||
        options.shard_index >= options.shard_count)
      return Error{ErrorKind::Other, "invalid shard"};
    return {};
  }

  // Settles what the endpoints can do, once the target root exists.
  void adapt_to_capabilities() {
    Capabilities src = source.capabilities();
    Capabilities dst = target.capabilities();
    mtime_resolution = std::max(src.mtime_resolution, dst.mtime_resolution);
    use_mtimes = dst.can_set_mtime;
    use_symlinks = dst.has_symlinks;
    symlink_owner = dst.symlink_owner;
    relax_modes = !dst.can_set_owner && options.preserve_mode;
    copy_options.preserve_mtime = use_mtimes;
    if (!use_mtimes)
      log::warn(target.describe(), " stores no mtimes: files are compared by size only");
    if (!use_symlinks) log::warn(target.describe(), " has no symlinks: symlinks are skipped");
    if (mtime_resolution > 1)
      log::info("comparing mtimes at a resolution of ", mtime_resolution, " ns");
  }

  // Prepares a node for a source directory: stats both sides and creates the
  // target directory if needed.
  Result<std::shared_ptr<DirNode>> make_node(const RelPath& path) {
    auto src = retry([&] { return source.stat(path); });
    if (!src.ok()) return src.error();
    if (src.value().type != EntryType::Directory)
      return Error{ErrorKind::NotADirectory, source.describe() + "/" + path + " is not a directory"};
    auto node = std::make_shared<DirNode>();
    node->path = path;
    node->source = src.value();
    node->owned = shard_owns(path);
    auto dst = retry([&] { return target.stat(path); });
    if (dst.ok()) {
      if (dst.value().type != EntryType::Directory)
        return Error{ErrorKind::Exists, target.describe() + "/" + path + " is not a directory"};
      node->target = dst.value();
    } else if (dst.error().kind == ErrorKind::NotFound) {
      Status created = create_dir(node);
      if (!created.ok()) return created.error();
    } else {
      return dst.error();
    }
    return node;
  }

  Status run_nodes(std::vector<std::shared_ptr<DirNode>> roots) {
    adapt_to_capabilities();
    if (journal) {
      for (const Failure& f : journal->failures()) prior_failures.insert(f.path);
    }
    {
      std::lock_guard lock(done_mutex);
      roots_pending = roots.size();
    }
    for (int i = 0; i < std::max(1, options.checkers); ++i)
      threads.emplace_back([this] { checker_loop(); });
    for (int i = 0; i < std::max(1, options.transfers); ++i)
      threads.emplace_back([this] { transfer_loop(); });
    for (auto& root : roots) dirs.push(root);

    {
      std::unique_lock lock(done_mutex);
      done_cv.wait(lock, [&] { return roots_pending == 0 || cancel.requested(); });
    }
    dirs.close(/*drain=*/true);
    copies.close(/*drain=*/true);
    for (auto& t : threads) t.join();
    threads.clear();
    if (cancel.requested()) return Error{ErrorKind::Cancelled, "run cancelled"};
    for (auto& root : roots)
      if (root->failed && !root->only)
        return Error{ErrorKind::Other, "could not process " + source.describe() + "/" + root->path};
    return {};
  }
};

Engine::Engine(Endpoint& source, Endpoint& target, SyncOptions options, Report& report,
               Journal* journal, const Cancellation& cancel)
    : impl_(std::make_unique<Impl>(source, target, std::move(options), report, journal, cancel)) {}

Engine::~Engine() = default;

Status Engine::run() {
  Status pre = impl_->preflight();
  if (!pre.ok()) return pre;
  auto root = impl_->make_node("");
  if (!root.ok()) return root.error();
  return impl_->run_nodes({root.value()});
}

Status Engine::run(const std::vector<Failure>& entries) {
  Status pre = impl_->preflight();
  if (!pre.ok()) return pre;

  // Entries below a failed directory are covered by that directory's walk.
  std::set<RelPath> failed_dirs;
  for (const Failure& f : entries)
    if (f.type == EntryType::Directory) failed_dirs.insert(f.path);

  // The rest is grouped by parent directory; each group becomes a node that
  // handles only those names, recursing into directories among them.
  std::map<RelPath, std::set<std::string>> groups;
  bool whole_tree = false;
  for (const Failure& f : entries) {
    bool covered = std::any_of(failed_dirs.begin(), failed_dirs.end(),
                               [&](const RelPath& d) { return is_under(f.path, d); });
    if (covered) continue;
    if (f.path.empty())
      whole_tree = true;
    else
      groups[parent_path(f.path)].insert(name_of(f.path));
  }
  if (whole_tree) return run();

  std::vector<std::shared_ptr<DirNode>> roots;
  for (auto& [dir, names] : groups) {
    auto node = impl_->make_node(dir);
    if (!node.ok()) {
      if (node.error().kind == ErrorKind::NotFound) {
        if (impl_->journal && !impl_->options.dry_run)
          for (const auto& name : names) impl_->journal->clear_failure(join(dir, name));
      } else {
        impl_->fail(dir, EntryType::Directory, node.error());
      }
      continue;
    }
    node.value()->only = std::move(names);
    roots.push_back(node.value());
  }
  return impl_->run_nodes(std::move(roots));
}

}  // namespace eosmirror
