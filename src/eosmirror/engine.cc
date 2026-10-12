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
#include <utility>

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
  // For the checker that processes the directory: the entries its listings
  // left out, and whether deletions in it are held back.
  size_t unlisted = 0;
  bool deletions_held = false;

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
  // One freed slot lets one waiter in. Waking all of them for each release
  // cost much system time with many small files and a limit below the
  // number of transfer threads.
  void release() {
    std::lock_guard lock(mutex_);
    --active_;
    cv_.notify_one();
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
  constexpr double kDecay = 0.995, kBetter = 1.05, kWorse = 0.9;
  constexpr int kHold = 6;
  byte_record_ *= kDecay;
  file_record_ *= kDecay;
  size_t raised_from = std::exchange(raised_from_, 0);
  bool holding = hold_ > 0;
  if (holding) --hold_;
  if (target_retries) {
    limit_ = next_transfer_limit(limit_, min_limit_, max_limit_, true, false);
    measuring_ = true;
    return limit_;
  }
  if (raised_from && byte_rate < byte_record_ * kWorse && file_rate < file_record_ * kWorse) {
    // The record is the throughput at the limit before.
    limit_ = raised_from;
    hold_ = kHold;
    return limit_;
  }
  bool improved = byte_rate > byte_record_ * kBetter || file_rate > file_record_ * kBetter;
  if (measuring_ || improved) {
    // The throughput at this limit is the one that a higher limit must beat.
    byte_record_ = byte_rate;
    file_record_ = file_rate;
  }
  if (improved && !measuring_ && !holding) {
    size_t raised = next_transfer_limit(limit_, min_limit_, max_limit_, false, true);
    if (raised > limit_) raised_from_ = limit_;
    limit_ = raised;
  }
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

  size_t max_transfers() const { return static_cast<size_t>(std::max(1, options.max_transfers)); }
  int checkers() const { return std::max(1, options.checkers); }
  size_t initial_limit() const {
    return std::min(max_transfers(), static_cast<size_t>(std::max(1, options.min_transfers)));
  }

  std::mutex done_mutex;
  std::condition_variable done_cv;
  size_t roots_pending = 0;

  std::mutex end_mutex;
  std::optional<Error> end_error;  // the run ends with this error, set at most once

  // The journal's failures by directory and name, with their kinds: to clear
  // on success, and to copy files again whose copy may have failed in place.
  std::map<RelPath, std::unordered_map<std::string, ErrorKind>> prior_failures;
  std::atomic<uint64_t> deletions{0};
  std::atomic<bool> delete_cap_reported{false};
  std::atomic<uint64_t> failures_in_a_row{0};
  std::atomic<int> busy_checkers{0};  // checkers processing a directory
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
    // Each deletion that the cap withholds is recorded, for --retry-failed,
    // but told once, and none is a failure in a row.
    if (error.kind == ErrorKind::Withheld) {
      bool first = !delete_cap_reported.exchange(true);
      if (first) log::warn(error.describe(), "; the deletions withheld after it are failures too, not logged");
      report.add_failure(std::move(f), false);
      return;
    }
    report.add_failure(std::move(f));
    in_a_row(path, type, error.describe());
  }

  // An endpoint that has become unusable would fail everything that is
  // left, and fill the journal with it; a source that has gone away (an
  // unmounted file system) makes everything that is left vanish.
  void in_a_row(const RelPath& path, EntryType type, const std::string& what) {
    if (failures_in_a_row.fetch_add(1) + 1 != options.max_consecutive_failures) return;
    std::string message = "stopped after " + std::to_string(options.max_consecutive_failures) +
                          " failures in a row (--max-consecutive-failures), the last: " +
                          std::string(to_string(type)) + " " + (path.empty() ? "." : path) +
                          ": " + what;
    log::error(message);
    end_run(Error{ErrorKind::Cancelled, message});
    cancel.request();
  }

  // Something worked: failures are no longer in a row.
  void healthy() {
    if (failures_in_a_row.load(std::memory_order_relaxed) != 0)
      failures_in_a_row.store(0, std::memory_order_relaxed);
  }

  // Drops listed entries whose names cannot be joined into paths. The number
  // of entries missing from the listing: those and the ones it skipped.
  size_t drop_invalid_names(const Endpoint& endpoint, const RelPath& dir, Listing& listing) {
    size_t dropped = std::erase_if(listing.entries, [&](const Entry& e) {
      if (valid_entry_name(e.name)) return false;
      stats.invalid_names.fetch_add(1);
      log::warn("skipping an entry with the invalid name \"", e.name, "\" in ", endpoint.describe(),
                "/", dir);
      return true;
    });
    return listing.skipped + dropped;
  }

  // Drops the journal's record of an earlier failure of the path.
  void succeeded(const RelPath& path) {
    healthy();
    forget_failure(path);
  }

  void forget_failure(const RelPath& path) {
    if (!journal || options.dry_run) return;
    auto rows = prior_failures.find(parent_path(path));
    if (rows != prior_failures.end() && rows->second.count(name_of(path)))
      journal->clear_failure(path);
  }

  // An entry that was listed on the source but is gone when it is handled:
  // the source changed, which is no failure, and the next run sees it gone.
  // Many in a row, though, mean that the source itself went away.
  void vanished(const RelPath& path, EntryType type) {
    stats.vanished.fetch_add(1);
    log::info(to_string(type), " ", path, " vanished from the source");
    forget_failure(path);
    in_a_row(path, type, "vanished from the source");
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
      for (const auto& [name, kind] : rows->second)
        if (gone(name)) journal->clear_failure(join(dir, name));
    for (; below != prior_failures.end() && below->first.starts_with(prefix); ++below) {
      std::string_view rest = std::string_view(below->first).substr(prefix.size());
      if (!gone(rest.substr(0, rest.find('/')))) continue;
      for (const auto& [name, kind] : below->second)
        journal->clear_failure(join(below->first, name));
    }
  }

  // Whether the journal records a failed copy of the file that may have left
  // it in place unverified, with the source's size and mtime: a transient
  // error other than a change of the source, as a failed verification or
  // close of an EOS upload whose file could not be taken back ends with.
  bool copy_in_doubt(const RelPath& path) const {
    auto rows = prior_failures.find(parent_path(path));
    if (rows == prior_failures.end()) return false;
    auto row = rows->second.find(name_of(path));
    return row != rows->second.end() && is_transient(row->second) &&
           row->second != ErrorKind::Changed;
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
    while (auto node = dirs.pop()) {
      busy_checkers.fetch_add(1);
      process_dir(*node);
      busy_checkers.fetch_sub(1);
    }
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
      if (listed.error().kind == ErrorKind::NotFound && node->parent) {
        vanished(node->path, EntryType::Directory);
        entry_done(node);
        return;
      }
      dir_failed(node, listed.error());
      return;
    }
    std::vector<Entry>& src_entries = listed.value().entries;
    node->unlisted = drop_invalid_names(source, node->path, listed.value());

    std::unordered_map<std::string, Entry> dst_entries;
    if (!node->created) {
      auto dst_listed = retry([&] { return target.list(node->path); });
      if (dst_listed.ok()) {
        node->unlisted += drop_invalid_names(target, node->path, dst_listed.value());
        for (Entry& e : dst_listed.value().entries) {
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

    // A source directory that is empty where the target's is not is more
    // likely an unmounted file system or a broken server than emptied. At
    // the top, that stops a run with --delete; below, see may_delete().
    bool emptied = empty_source(src_entries) && !empty_target(dst_entries);
    if (options.delete_extra && emptied && !node->parent && !node->only) {
      node->failed = true;
      end_run(Error{ErrorKind::Other, source.describe() + " is empty, but " + target.describe() +
                                          " is not: refusing to delete everything there"});
      entry_done(node);
      return;
    }

    for (Entry& e : src_entries) {
      if (cancel.requested()) break;
      if (node->only && !node->only->count(e.name)) continue;
      if (source.is_temporary(e)) {
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
      bool removable = std::any_of(dst_entries.begin(), dst_entries.end(), [&](const auto& e) {
        return !target.is_temporary(e.second) || stale(e.second);
      });
      handle_extras(node, dst_entries,
                    options.delete_extra && removable && may_delete(node, emptied) &&
                        absent_on_source(node, dst_entries));
    }
    entry_done(node);
  }

  // Whether a temporary on the target is a leftover that nobody works on:
  // neither its mtime nor its ctime is recent. EOS sets an atomic upload's
  // mtime to the source's when it creates it (eos.mtime), so its ctime tells
  // when it was created.
  bool stale(const Entry& temporary) const {
    int64_t now = std::chrono::duration_cast<std::chrono::seconds>(
                      std::chrono::system_clock::now().time_since_epoch())
                      .count();
    return std::max(temporary.mtime, temporary.ctime).sec + options.stale_temp_age.count() < now;
  }

  // Whether --delete may delete the extras of a directory: not where
  // deletions are held back, and not in a directory emptied on the source
  // unless --delete-emptied says so.
  bool may_delete(const std::shared_ptr<DirNode>& node, bool emptied) {
    if (!deletions_allowed(node)) return false;
    if (!emptied || options.delete_emptied) return true;
    stats.emptied_dirs.fetch_add(1);
    log::warn("deleting nothing in ", node->path.empty() ? "." : node->path,
              ": the source's directory is empty, the target's is not (--delete-emptied "
              "deletes there)");
    return false;
  }

  // Whether entries of a directory may be deleted, so far: not where a
  // listing left out entries, whose names the other side may list as
  // entries it lacks.
  bool deletions_allowed(const std::shared_ptr<DirNode>& node) {
    if (node->unlisted > 0)
      hold_deletions(node, std::to_string(node->unlisted) +
                               " entries could not be listed safely, on one side or the other");
    return !node->deletions_held;
  }

  // Whether the source lacks the extras that would be deleted, as its
  // listing says, by a stat of each: a listing cut short without saying so
  // would otherwise delete what it left out. Holds back the deletions if not.
  bool absent_on_source(const std::shared_ptr<DirNode>& node,
                        const std::unordered_map<std::string, Entry>& extras) {
    for (const auto& [name, e] : extras) {
      if (cancel.requested()) return false;
      if (target.is_temporary(e) && !stale(e)) continue;
      RelPath path = join(node->path, name);
      auto src = retry([&] { return source.stat(path); });
      if (src.ok() ? source.is_temporary(src.value()) : src.error().kind == ErrorKind::NotFound)
        continue;
      hold_deletions(node, src.ok() ? path + " is on the source, though its listing lacks it"
                                    : "cannot tell whether " + path + " is on the source: " +
                                          src.error().describe());
      return false;
    }
    return true;
  }

  // Deletes nothing more in a directory whose listings may be incomplete;
  // counted and told once.
  void hold_deletions(const std::shared_ptr<DirNode>& node, const std::string& why) {
    if (node->deletions_held) return;
    node->deletions_held = true;
    stats.incomplete_dirs.fetch_add(1);
    log::warn("deleting nothing in ", node->path.empty() ? "." : node->path, ": ", why);
  }

  bool empty_source(const std::vector<Entry>& entries) const {
    return std::all_of(entries.begin(), entries.end(),
                       [&](const Entry& e) { return source.is_temporary(e); });
  }

  bool empty_target(const std::unordered_map<std::string, Entry>& entries) const {
    return std::all_of(entries.begin(), entries.end(),
                       [&](const auto& e) { return target.is_temporary(e.second); });
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
    if (!deletions_allowed(node)) {
      fail(path, wanted,
           Error{ErrorKind::Exists, "target is a " + std::string(to_string(existing.type)) +
                                        ", not replaced while deletions in its directory are "
                                        "held back"});
      return false;
    }
    // A listing that shows the source's entry with another type than it has
    // is wrong, and so would be the deletion.
    auto src = retry([&] { return source.stat(path); });
    if (!src.ok() && src.error().kind == ErrorKind::NotFound) {
      vanished(path, wanted);
      return false;
    }
    if (!src.ok() || src.value().type != wanted) {
      Error e = src.ok() ? Error{ErrorKind::Changed,
                                 "listed as a " + std::string(to_string(wanted)) +
                                     " on the source, but a " +
                                     std::string(to_string(src.value().type)) + " there now"}
                         : src.error();
      hold_deletions(node, "cannot confirm the type of " + path + " on the source: " +
                               e.describe());
      fail(path, wanted, e);
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
      // The shard that owns the directory replaces the entry; until then,
      // this one cannot walk the subtree, which a later run does.
      if (!node->owned) {
        fail(path, EntryType::Directory,
             Error{ErrorKind::Exists, "target is a " + std::string(to_string(existing->type)) +
                                          ", which the shard that owns its directory replaces: "
                                          "not walked in this run"});
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
    bool in_doubt = existing && copy_in_doubt(path);
    if (existing && !in_doubt && existing->size == e.size &&
        (!use_mtimes || same_mtime(existing->mtime, e.mtime))) {
      stats.files_unchanged.fetch_add(1);
      MetaFields fields = differences(e, *existing);
      if (!apply_metadata(path, e, fields)) return;
      if (fields != MetaFields::None) stats.metadata_fixed.fetch_add(1);
      succeeded(path);
      return;
    }
    ensure_writable(node);
    if (in_doubt && !invalidate_in_doubt(path, e)) return;
    node->pending.fetch_add(1);
    stats.queued_copies.fetch_add(1);
    if (!copies.push(CopyJob{node, e, existing != nullptr})) {
      stats.queued_copies.fetch_sub(1);
      entry_done(node);
    }
  }

  // Gives a file whose last copy is in doubt mtime 0 before it is copied
  // again, so that an upload that does not arrive (EOS confirms commits by
  // size and mtime where it has no checksum) cannot be taken for it. Whether
  // that worked.
  bool invalidate_in_doubt(const RelPath& path, const Entry& e) {
    log::debug("copying ", path, " again: its last copy failed");
    if (!use_mtimes || options.dry_run) return true;
    Entry md = e;
    md.mtime = {0, 0};
    Status reset = retry([&] { return target.set_metadata(path, md, MetaFields::Mtime); });
    if (reset.ok()) return true;
    // Still in doubt for the next run, whatever the error.
    Error error = reset.error();
    error.kind = ErrorKind::IO;
    fail(path, EntryType::File, error);
    return false;
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

  // Counts the target's entries that the source lacks, and deletes them,
  // stale temporaries included, if deleting.
  void handle_extras(const std::shared_ptr<DirNode>& node,
                     const std::unordered_map<std::string, Entry>& extras, bool deleting) {
    for (const auto& [name, e] : extras) {
      if (cancel.requested()) return;
      RelPath path = join(node->path, name);
      if (target.is_temporary(e)) {
        if (!stale(e)) continue;
        if (!deleting) {
          stats.stale_temps.fetch_add(1);
          continue;
        }
      } else {
        stats.extras.fetch_add(1);
        if (!deleting) {
          log::debug("extra entry on target: ", path);
          continue;
        }
      }
      ensure_writable(node);
      node->touched = true;
      Status removed = delete_tree(path, e);
      if (!removed.ok()) fail(path, e.type, removed.error());
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
      // Entries left out of the listing would remain, and their names may
      // be on the source.
      if (size_t unlisted = drop_invalid_names(target, path, listed.value()); unlisted > 0)
        return Error{ErrorKind::NotEmpty, "not deleting " + path + ": " +
                                              std::to_string(unlisted) +
                                              " of its entries could not be listed safely"};
      for (const Entry& child : listed.value().entries) {
        if (cancel.requested()) return Error{ErrorKind::Cancelled, "cancelled"};
        Status s = delete_tree(join(path, child.name), child);
        if (!s.ok()) return s;
      }
    }
    if (deletions.fetch_add(1) >= options.max_delete) {
      deletions.fetch_sub(1);
      return Error{ErrorKind::Withheld, "deletion cap of " + std::to_string(options.max_delete) +
                                            " reached (--max-delete), not deleting " + path};
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
    // Waiting for copies while every checker is busy means that listing
    // bounds the run: more checkers may help.
    auto listing_bound = [&] {
      if (busy_checkers.load() == checkers()) stats.listing_bound.fetch_add(1);
    };
    while (auto job = copies.pop(listing_bound)) {
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
      bool target_failed = false;
      job_options.on_target_error = [&](const Error& e) {
        target_failed = true;
        if (is_transient(e.kind)) target_errors.fetch_add(1);
      };
      // The error of an attempt that may have left its copy in place.
      std::optional<Error> doubt;
      auto result = retry([&] {
        slot.written = 0;
        target_failed = false;
        auto copied = copy_file(source, target, path, job.source, job_options, pool, cancel);
        if (!copied.ok() && copied.error().in_doubt) doubt = copied.error();
        return copied;
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
      } else if (doubt) {
        // The journal keeps the copy in doubt, however its last attempt
        // ended, also when cancelled.
        Error e = result.error();
        if (e.kind == ErrorKind::Cancelled)
          e = *doubt;
        else if (!e.in_doubt)
          e = Error{ErrorKind::IO, e.describe() + "; an attempt before: " + doubt->describe()};
        fail(path, EntryType::File, e);
      } else if (result.error().kind == ErrorKind::NotFound && !target_failed) {
        vanished(path, EntryType::File);
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
    if (std::string note = target.target_note(); !note.empty()) log::info(note);
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
        prior_failures[parent_path(f.path)][name_of(f.path)] = f.error.kind;
    }
    {
      std::lock_guard lock(done_mutex);
      roots_pending = roots.size();
    }
    size_t transfers = max_transfers();
    for (int i = 0; i < checkers(); ++i) threads.emplace_back([this] { checker_loop(); });
    for (size_t i = 0; i < transfers; ++i)
      threads.emplace_back([this, i] { transfer_loop(i); });
    if (initial_limit() < transfers) controller = std::thread([this] { controller_loop(); });
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
