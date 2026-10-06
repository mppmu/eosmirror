// SPDX-License-Identifier: GPL-3.0-or-later
#include "eosmirror/engine.hh"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string_view>
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
  std::atomic<bool> relaxing_tried{false};
  std::atomic<bool> made_writable{false};  // the target's mode was relaxed for changes
  std::atomic<bool> unverified_warned{false};
  std::mutex writable_mutex;
};

struct CopyJob {
  std::shared_ptr<DirNode> dir;
  Entry source;
  bool replaces = true;  // the target had a file of that name when listed
};

// Limits how many transfers run at once; the limit can change while
// transfers wait.
class Gate {
 public:
  explicit Gate(size_t limit) : limit_(limit) {}

  bool acquire() {
    std::unique_lock lock(mutex_);
    cv_.wait(lock, [&] { return closed_ || active_ < limit_; });
    if (closed_) return false;
    ++active_;
    return true;
  }
  void release() {
    std::lock_guard lock(mutex_);
    --active_;
    cv_.notify_all();
  }
  void set_limit(size_t limit) {
    std::lock_guard lock(mutex_);
    limit_ = limit;
    cv_.notify_all();
  }
  size_t limit() const {
    std::lock_guard lock(mutex_);
    return limit_;
  }
  void close() {
    std::lock_guard lock(mutex_);
    closed_ = true;
    cv_.notify_all();
  }

 private:
  mutable std::mutex mutex_;
  std::condition_variable cv_;
  size_t limit_;
  size_t active_ = 0;
  bool closed_ = false;
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

}  // namespace

size_t next_transfer_limit(size_t limit, size_t min_limit, size_t max_limit, bool retried,
                           bool improved) {
  if (retried) return std::max(min_limit, limit - std::min(limit, std::max<size_t>(1, limit / 4)));
  if (improved) return std::min(max_limit, limit + std::max<size_t>(2, limit / 2));
  return limit;
}

TransferController::TransferController(size_t min_limit, size_t max_limit)
    : min_limit_(min_limit), max_limit_(std::max(min_limit, max_limit)), limit_(min_limit) {}

size_t TransferController::update(double byte_rate, double file_rate, bool target_retries) {
  constexpr double kDecay = 0.995, kMargin = 1.05;
  byte_record_ *= kDecay;
  file_record_ *= kDecay;
  if (target_retries) {
    limit_ = next_transfer_limit(limit_, min_limit_, max_limit_, true, false);
    measuring_ = true;
    return limit_;
  }
  bool improved = byte_rate > byte_record_ * kMargin || file_rate > file_record_ * kMargin;
  if (measuring_ || improved) {
    // The throughput at this limit is the one that a higher limit must beat.
    byte_record_ = byte_rate;
    file_record_ = file_rate;
  }
  if (improved && !measuring_)
    limit_ = next_transfer_limit(limit_, min_limit_, max_limit_, false, true);
  measuring_ = false;
  return limit_;
}

struct Engine::Impl {
  Impl(Endpoint& src, Endpoint& dst, SyncOptions opts, Report& rep, Journal* jnl,
       const Cancellation& cnl)
      : source(src),
        target(dst),
        options(std::move(opts)),
        report(rep),
        stats(rep.stats),
        journal(jnl),
        cancel(&cnl),
        dirs(std::numeric_limits<size_t>::max(), /*lifo=*/true),
        copies(std::max<size_t>(1, options.max_backlog)),
        gate(initial_limit()) {
    copy_options.preserve_owner = options.preserve_owner;
    copy_options.preserve_mode = options.preserve_mode;
    copy_options.verify = options.verify || options.require_checksum;
    copy_options.require_verification = options.require_checksum;
    // Before a display can look at them.
    report.init_slots(max_transfers(), options.max_backlog);
    stats.transfer_limit.store(gate.limit());
  }

  Endpoint& source;
  Endpoint& target;
  SyncOptions options;
  Report& report;
  Stats& stats;
  Journal* journal;
  Cancellation cancel;  // the caller's, or the engine's own when it ends the run
  CopyOptions copy_options;

  // Settled once the target root exists, since probing it may need that.
  int32_t mtime_resolution = 1;
  bool use_mtimes = true;    // the target stores mtimes: compare and set them
  bool use_symlinks = true;  // the target has symlinks
  bool symlink_owner = true;  // symlinks on the target have settable owners
  ModeBits file_mode_bits = 07777;  // the mode bits the target stores
  ModeBits dir_mode_bits = 07777;
  // An endpoint normally provides a checksum to verify copies against: the
  // target computes them, or else the source stores them.
  bool checksums_expected = false;
  bool relax_modes = false;  // make read-only target directories writable first

  WorkQueue<std::shared_ptr<DirNode>> dirs;
  WorkQueue<CopyJob> copies;
  Gate gate;
  std::vector<std::thread> threads;
  std::thread controller;
  std::atomic<bool> controller_stop{false};
  bool ran = false;

  size_t max_transfers() const { return static_cast<size_t>(std::max(1, options.transfers)); }
  size_t initial_limit() const {
    if (!options.adaptive) return max_transfers();
    return std::min(max_transfers(), static_cast<size_t>(std::max(1, options.min_transfers)));
  }

  std::mutex done_mutex;
  std::condition_variable done_cv;
  size_t roots_pending = 0;

  std::mutex end_mutex;
  std::optional<Error> end_error;  // the run ends with this error, set at most once

  // The names of the journal's failures by directory, to clear on success.
  std::map<RelPath, std::unordered_set<std::string>> prior_failures;
  std::atomic<uint64_t> deletions{0};
  std::atomic<bool> delete_cap_reported{false};
  std::atomic<uint64_t> failures_in_a_row{0};
  // Transient errors of transfers on the target, which is how an overloaded
  // target shows.
  std::atomic<uint64_t> target_errors{0};

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

  // Ends the run with the error once the workers are done; the first one
  // counts.
  void end_run(Error error) {
    std::lock_guard lock(end_mutex);
    if (!end_error) end_error = std::move(error);
  }

  void fail(const RelPath& path, EntryType type, const Error& error) {
    if (error.kind == ErrorKind::Cancelled) return;
    Failure f{path, type, error};
    if (journal && !options.dry_run) journal->record_failure(f);
    report.add_failure(std::move(f));
    // An endpoint that has become unusable would fail everything that is
    // left, and fill the journal with it.
    if (failures_in_a_row.fetch_add(1) + 1 == options.max_consecutive_failures) {
      std::string message = "stopped after " + std::to_string(options.max_consecutive_failures) +
                            " failures in a row (--max-consecutive-failures), the last: " +
                            std::string(to_string(type)) + " " + (path.empty() ? "." : path) +
                            ": " + error.describe();
      log::error(message);
      end_run(Error{ErrorKind::Cancelled, message});
      cancel.request();
    }
  }

  // Something worked: failures are no longer in a row.
  void healthy() {
    if (failures_in_a_row.load(std::memory_order_relaxed) != 0)
      failures_in_a_row.store(0, std::memory_order_relaxed);
  }

  // Drops listed entries whose names cannot be joined into paths.
  void drop_invalid_names(const Endpoint& endpoint, const RelPath& dir,
                          std::vector<Entry>& entries) {
    std::erase_if(entries, [&](const Entry& e) {
      if (valid_entry_name(e.name)) return false;
      stats.invalid_names.fetch_add(1);
      log::warn("skipping an entry with the invalid name \"", e.name, "\" in ", endpoint.describe(),
                "/", dir);
      return true;
    });
  }

  // Drops the journal's record of an earlier failure of the path.
  void succeeded(const RelPath& path) {
    healthy();
    if (!journal || options.dry_run) return;
    auto rows = prior_failures.find(parent_path(path));
    if (rows != prior_failures.end() && rows->second.count(name_of(path)))
      journal->clear_failure(path);
  }

  // Drops the journal's records of earlier failures of entries in a
  // directory, or below them, whose names are gone from both sides, which
  // leaves nothing to retry.
  void clear_vanished(const RelPath& dir, const std::vector<Entry>& src_entries,
                      const std::unordered_map<std::string, Entry>& dst_extras) {
    if (!journal || options.dry_run) return;
    std::string prefix = dir.empty() ? "" : dir + "/";
    auto rows = prior_failures.find(dir);
    auto below = prior_failures.lower_bound(prefix);
    if (below != prior_failures.end() && below->first == dir) ++below;  // dir "" itself
    bool any_below = below != prior_failures.end() && below->first.starts_with(prefix);
    if (rows == prior_failures.end() && !any_below) return;

    std::unordered_set<std::string_view> in_source;
    for (const Entry& e : src_entries) in_source.insert(e.name);
    auto gone = [&](std::string_view name) {
      return !name.empty() && !in_source.count(name) && !dst_extras.count(std::string(name));
    };
    if (rows != prior_failures.end())
      for (const std::string& name : rows->second)
        if (gone(name)) journal->clear_failure(join(dir, name));
    for (; below != prior_failures.end() && below->first.starts_with(prefix); ++below) {
      std::string_view rest = std::string_view(below->first).substr(prefix.size());
      if (!gone(rest.substr(0, rest.find('/')))) continue;
      for (const std::string& name : below->second)
        journal->clear_failure(join(below->first, name));
    }
  }

  // The metadata fields of the target entry that differ from the source.
  MetaFields differences(const Entry& src, const Entry& dst) const {
    MetaFields fields = MetaFields::None;
    bool owner_matters =
        options.preserve_owner && (src.type != EntryType::Symlink || symlink_owner);
    if (owner_matters && (src.uid != dst.uid || src.gid != dst.gid))
      fields = fields | MetaFields::Owner;
    bool mode_matters = options.preserve_mode && src.type != EntryType::Symlink;
    ModeBits bits = src.type == EntryType::Directory ? dir_mode_bits : file_mode_bits;
    if (mode_matters && (src.mode & bits) != (dst.mode & bits)) fields = fields | MetaFields::Mode;
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

  // Before changing an existing target directory that may not be writable
  // for us (its entries, or its mtime), adds the owner's rwx bits to its
  // mode; finalize restores the mode.
  void ensure_writable(const std::shared_ptr<DirNode>& node) {
    if (!relax_modes || node->created || options.dry_run || (node->target.mode & 0700) == 0700 ||
        node->relaxing_tried)
      return;
    std::lock_guard lock(node->writable_mutex);
    if (node->relaxing_tried) return;
    Entry relaxed = node->target;
    relaxed.mode |= 0700;
    Status s = retry([&] { return target.set_metadata(node->path, relaxed, MetaFields::Mode); });
    if (s.ok())
      node->made_writable = true;
    else
      log::warn("cannot make ", node->path, " writable: ", s.error().describe());
    node->relaxing_tried = true;
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
        // Setting the mtime takes write access as well (EOS checks it).
        if (has(fields, MetaFields::Mtime)) ensure_writable(node);
        if (node->made_writable && options.preserve_mode) fields = fields | MetaFields::Mode;
      }
      if (!apply_metadata(node->path, node->source, fields))
        node->failed = true;
      else if (!node->created && fields != MetaFields::None)
        stats.metadata_fixed.fetch_add(1);
    } else if (!cancel.requested() && !node->failed && node->made_writable) {
      // The shard that owns the directory sets its metadata, but the mode
      // relaxed for creating a subdirectory is restored here.
      if (!apply_metadata(node->path, node->source, MetaFields::Mode)) node->failed = true;
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
      dir_failed(node, listed.error());
      return;
    }
    std::vector<Entry>& src_entries = listed.value();
    drop_invalid_names(source, node->path, src_entries);

    std::unordered_map<std::string, Entry> dst_entries;
    if (!node->created) {
      auto dst_listed = retry([&] { return target.list(node->path); });
      if (dst_listed.ok()) {
        drop_invalid_names(target, node->path, dst_listed.value());
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
          dir_failed(node, created.error());
          return;
        }
      }
    }
    stats.dirs_listed.fetch_add(1);
    healthy();

    if (options.delete_extra && !node->parent && !node->only && empty_source(src_entries) &&
        !empty_target(dst_entries)) {
      // More likely an unmounted file system or a broken server than a tree
      // whose whole copy is to be deleted.
      node->failed = true;
      end_run(Error{ErrorKind::Other, source.describe() + " is empty, but " + target.describe() +
                                          " is not: refusing to delete everything there"});
      entry_done(node);
      return;
    }

    for (Entry& e : src_entries) {
      if (cancel.requested()) break;
      if (node->only && !node->only->count(e.name)) continue;
      if (source.is_temporary(e.name)) {
        log::debug("skipping the source's temporary ", join(node->path, e.name));
        continue;
      }
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

    // What is left of the target's entries is not in the source.
    clear_vanished(node->path, src_entries, dst_entries);
    if (node->owned && !cancel.requested()) {
      // A retry handles only its own names, among them failed deletions.
      if (node->only)
        std::erase_if(dst_entries, [&](const auto& e) { return !node->only->count(e.first); });
      handle_extras(node, dst_entries);
    }
    entry_done(node);
  }

  bool empty_source(const std::vector<Entry>& entries) const {
    return std::all_of(entries.begin(), entries.end(),
                       [&](const Entry& e) { return source.is_temporary(e.name); });
  }

  bool empty_target(const std::unordered_map<std::string, Entry>& entries) const {
    return std::all_of(entries.begin(), entries.end(),
                       [&](const auto& e) { return target.is_temporary(e.first); });
  }

  // Reports a directory that could not be listed or created. For the top
  // directory of a run, that ends the run.
  void dir_failed(const std::shared_ptr<DirNode>& node, const Error& error) {
    node->failed = true;
    fail(node->path, EntryType::Directory, error);
    if (!node->parent && !node->only && error.kind != ErrorKind::Cancelled)
      end_run(Error{error.kind, "could not process the top directory: " + error.describe()});
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
    // Listings that follow symlinks can lead back into an ancestor.
    if (!e.id.empty()) {
      for (const DirNode* a = node.get(); a; a = a->parent.get()) {
        if (a->source.id != e.id) continue;
        fail(path, EntryType::Directory,
             Error{ErrorKind::Other, "directory cycle: " + path + " is " +
                                         (a->path.empty() ? "the top directory" : a->path)});
        return;
      }
    }
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
    stats.queued_copies.fetch_add(1);
    if (!copies.push(CopyJob{node, e, existing != nullptr})) {
      stats.queued_copies.fetch_sub(1);
      entry_done(node);
    }
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
    if (options.dry_run) {
      stats.symlinks_created.fetch_add(1);
      return;
    }
    Status s = retry([&] { return target.symlink(path, link_target); });
    if (!s.ok()) {
      fail(path, EntryType::Symlink, s.error());
      return;
    }
    stats.symlinks_created.fetch_add(1);
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
      if (!removed.ok() && (!removed.error().message.starts_with("deletion cap") ||
                            !delete_cap_reported.exchange(true)))
        fail(path, e.type, removed.error());
    }
  }

  // Deletes a target entry, directories recursively, within the deletion
  // cap. Reports no failures but returns the error of the first entry that
  // could not be removed.
  Status delete_tree(const RelPath& path, const Entry& e) {
    if (e.type == EntryType::Directory) {
      // Without the owner's rwx bits, its entries can be neither listed nor
      // removed. The mode need not be restored.
      if (relax_modes && !options.dry_run && (e.mode & 0700) != 0700) {
        Entry relaxed = e;
        relaxed.mode |= 0700;
        Status s = retry([&] { return target.set_metadata(path, relaxed, MetaFields::Mode); });
        if (!s.ok()) return s;
      }
      auto listed = retry([&] { return target.list(path); });
      if (!listed.ok()) return listed.error();
      drop_invalid_names(target, path, listed.value());
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
    succeeded(path);
    return {};
  }

  // ---- transfers -----------------------------------------------------------------

  void transfer_loop(size_t index) {
    BufferPool pool(std::max<size_t>(options.buffer_size, 4096));
    TransferSlot& slot = *report.slots[index];
    while (auto job = copies.pop()) {
      stats.queued_copies.fetch_sub(1);
      if (gate.acquire()) {
        run_copy(*job, pool, slot);
        gate.release();
      }
      // Finalizing the directory takes no transfer slot.
      entry_done(job->dir);
    }
  }

  // Adjusts the transfer limit every interval, see TransferController.
  void controller_loop() {
    TransferController limits(initial_limit(), max_transfers());
    uint64_t last_bytes = stats.bytes_written.load();
    uint64_t last_files = stats.files_copied.load();
    uint64_t last_errors = target_errors.load();
    auto last = std::chrono::steady_clock::now();
    while (!controller_stop) {
      if (cancel.wait(options.adapt_interval, &controller_stop)) break;
      auto now = std::chrono::steady_clock::now();
      double secs = std::chrono::duration<double>(now - last).count();
      if (secs <= 0) continue;
      last = now;
      uint64_t bytes = stats.bytes_written.load(), files = stats.files_copied.load();
      uint64_t errors = target_errors.load();
      double byte_rate = static_cast<double>(bytes - last_bytes) / secs;
      double file_rate = static_cast<double>(files - last_files) / secs;
      bool retried = errors != last_errors;
      last_bytes = bytes;
      last_files = files;
      last_errors = errors;

      size_t limit = limits.limit();
      size_t wanted = limits.update(byte_rate, file_rate, retried);
      if (wanted != limit) {
        log::info("transfer limit ", limit, " -> ", wanted, " (",
                  format_bytes(static_cast<uint64_t>(byte_rate)), "/s, ",
                  static_cast<uint64_t>(file_rate), " files/s",
                  retried ? ", target errors" : "", ")");
        gate.set_limit(wanted);
        stats.transfer_limit.store(wanted);
      }
    }
  }

  void run_copy(const CopyJob& job, BufferPool& pool, TransferSlot& slot) {
    RelPath path = join(job.dir->path, job.source.name);
    job.dir->touched = true;
    if (options.dry_run) {
      stats.files_copied.fetch_add(1);
      stats.bytes_copied.fetch_add(job.source.size);
      log::debug("would copy ", path);
    } else if (!cancel.requested()) {
      {
        std::lock_guard lock(slot.mutex);
        slot.path = path;
        slot.size = job.source.size;
      }
      slot.written = 0;
      slot.active = true;
      CopyOptions job_options = copy_options;
      job_options.replaces = job.replaces;
      job_options.on_chunk = [&](uint64_t n) {
        stats.bytes_written.fetch_add(n);
        slot.written.fetch_add(n);
      };
      job_options.on_target_error = [&](const Error& e) {
        if (is_transient(e.kind)) target_errors.fetch_add(1);
      };
      auto result = retry([&] {
        slot.written = 0;
        return copy_file(source, target, path, job.source, job_options, pool, cancel);
      });
      slot.active = false;
      if (result.ok()) {
        stats.files_copied.fetch_add(1);
        stats.bytes_copied.fetch_add(result.value().bytes);
        log::debug("copied ", path, " (", result.value().bytes, " bytes)");
        if (copy_options.verify && !result.value().verified) {
          stats.files_unverified.fetch_add(1);
          // Worth a warning where copies are normally verified (an EOS
          // directory without checksums), not between local file systems.
          if (checksums_expected && !job.dir->unverified_warned.exchange(true))
            log::warn("no checksum verification for files copied into ",
                      job.dir->path.empty() ? "." : job.dir->path);
        }
        succeeded(path);
      } else {
        fail(path, EntryType::File, result.error());
      }
    }
  }

  // ---- running ----------------------------------------------------------------------

  Status preflight() {
    if (ran) return Error{ErrorKind::Other, "an engine runs only once"};
    ran = true;
    if (std::string warning = target.target_warning(); !warning.empty()) log::warn(warning);
    if (options.preserve_owner && !source.capabilities().has_owners) {
      log::warn(source.describe(), " reports no owners: owners and groups are not synchronized");
      options.preserve_owner = false;
      copy_options.preserve_owner = false;
    }
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
    if (!options.dry_run) target.probe_target();
    Capabilities src = source.capabilities();
    Capabilities dst = target.capabilities();
    mtime_resolution = std::max(src.mtime_resolution, dst.mtime_resolution);
    use_mtimes = dst.can_set_mtime;
    use_symlinks = dst.has_symlinks;
    symlink_owner = dst.symlink_owner;
    file_mode_bits = dst.file_mode_bits;
    dir_mode_bits = dst.dir_mode_bits;
    checksums_expected = dst.checksum != ChecksumType::None || src.checksum != ChecksumType::None;
    if (options.preserve_mode && (file_mode_bits != 07777 || dir_mode_bits != 07777)) {
      char bits[80];
      std::snprintf(bits, sizeof bits, "%04o of files and %04o of directories", file_mode_bits,
                    dir_mode_bits);
      log::info(target.describe(), " stores only the mode bits ", bits);
    }
    relax_modes = !dst.can_set_owner && options.preserve_mode;
    copy_options.preserve_mtime = use_mtimes;
    if (!use_mtimes)
      log::warn(target.describe(), " stores no mtimes: files are compared by size only");
    if (!use_symlinks) log::warn(target.describe(), " has no symlinks: symlinks are skipped");
    if (mtime_resolution > 1)
      log::info("comparing mtimes at a resolution of ", mtime_resolution, " ns");
  }

  // The source's entry of a directory that a run starts from.
  Result<Entry> source_dir(const RelPath& path) {
    auto src = retry([&] { return source.stat(path); });
    if (!src.ok()) return src.error();
    if (src.value().type != EntryType::Directory)
      return Error{ErrorKind::NotADirectory, source.describe() + "/" + path + " is not a directory"};
    return src;
  }

  // Prepares a node for a source directory: stats the target and creates the
  // target directory if needed.
  Result<std::shared_ptr<DirNode>> make_node(const RelPath& path, const Entry& src) {
    auto node = std::make_shared<DirNode>();
    node->path = path;
    node->source = src;
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

  // Synchronizes the whole tree.
  Status run_tree() {
    auto src = source_dir("");
    if (!src.ok()) return src.error();
    auto root = make_node("", src.value());
    if (!root.ok()) return root.error();
    return run_nodes({root.value()});
  }

  Status run_nodes(std::vector<std::shared_ptr<DirNode>> roots) {
    adapt_to_capabilities();
    if (journal) {
      for (const Failure& f : journal->failures())
        prior_failures[parent_path(f.path)].insert(name_of(f.path));
    }
    {
      std::lock_guard lock(done_mutex);
      roots_pending = roots.size();
    }
    size_t transfers = max_transfers();
    for (int i = 0; i < std::max(1, options.checkers); ++i)
      threads.emplace_back([this] { checker_loop(); });
    for (size_t i = 0; i < transfers; ++i)
      threads.emplace_back([this, i] { transfer_loop(i); });
    if (options.adaptive && initial_limit() < transfers)
      controller = std::thread([this] { controller_loop(); });
    for (auto& root : roots) dirs.push(root);

    {
      std::unique_lock lock(done_mutex);
      done_cv.wait(lock, [&] { return roots_pending == 0 || cancel.requested(); });
    }
    controller_stop = true;
    dirs.close(/*drain=*/true);
    copies.close(/*drain=*/true);
    gate.close();
    for (auto& t : threads) t.join();
    threads.clear();
    if (controller.joinable()) controller.join();
    if (end_error) return *end_error;
    if (cancel.requested()) return Error{ErrorKind::Cancelled, "run cancelled"};
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
  return impl_->run_tree();
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
    bool covered = false;
    for (RelPath dir = f.path; !dir.empty() && !covered;) {
      dir = parent_path(dir);
      covered = failed_dirs.count(dir) > 0;
    }
    if (covered) continue;
    if (f.path.empty())
      whole_tree = true;
    else
      groups[parent_path(f.path)].insert(name_of(f.path));
  }
  if (whole_tree) return impl_->run_tree();

  std::vector<std::shared_ptr<DirNode>> roots;
  for (auto& [dir, names] : groups) {
    auto src = impl_->source_dir(dir);
    if (!src.ok() && src.error().kind == ErrorKind::NotFound) {
      // Gone from the source: nothing left to retry.
      if (impl_->journal && !impl_->options.dry_run)
        for (const auto& name : names) impl_->journal->clear_failure(join(dir, name));
      continue;
    }
    auto node = src.ok() ? impl_->make_node(dir, src.value())
                         : Result<std::shared_ptr<DirNode>>(src.error());
    if (!node.ok()) {
      impl_->fail(dir, EntryType::Directory, node.error());
      continue;
    }
    node.value()->only = std::move(names);
    roots.push_back(node.value());
  }
  return impl_->run_nodes(std::move(roots));
}

}  // namespace eosmirror
