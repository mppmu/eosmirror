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
  bool replaces_at_open = false;  // the target replaces files at their open (Listing)

  std::atomic<int> pending{1};  // own processing plus unfinished entries
  std::atomic<bool> touched{false};        // entries were created, replaced or removed
  std::atomic<bool> failed{false};         // listing, creation or metadata failed
  // Set when a failure of the directory is recorded while its entries and
  // metadata are handled (entries left out of a listing, deletions held
  // back). Its completion must not clear that failure.
  std::atomic<bool> incomplete{false};
  std::atomic<bool> relaxing_tried{false};
  std::atomic<bool> made_writable{false};  // the target's mode was relaxed for changes
  std::atomic<bool> unverified_warned{false};
  std::atomic<bool> replacing_warned{false};
  std::mutex writable_mutex;
};

struct CopyJob {
  std::shared_ptr<DirNode> dir;
  Entry source;
  bool replaces = true;  // the target had a file of that name when listed
  // The target's file of the source's size, to compare by checksum before
  // copying (--checksum).
  std::optional<Entry> compare;
};

// Limits how many transfers run at once. The limit can change while
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
  bool confirms_after_commit = false;  // the target's commits are confirmed afterwards
  bool relax_modes = false;  // make read-only target directories writable first
  // Removing a target file may remove what a symlink in its place points to
  // (Capabilities::follows_symlinks), unless the target has none.
  bool removes_safely = true;

  WorkQueue<std::shared_ptr<DirNode>> dirs;
  WorkQueue<CopyJob> copies;
  Gate gate;
  std::vector<std::thread> threads;
  std::thread controller;
  std::atomic<bool> controller_stop{false};
  std::thread syncer;  // makes copies durable every sync_interval, until stopped
  std::mutex syncer_mutex;
  std::condition_variable syncer_cv;
  bool syncer_stop = false;
  // Directories finalized since the last sync, to record as finalized after
  // the next one: a copy may need that sync to become durable and final
  // (PosixOptions::sync).
  std::mutex finalized_mutex;
  std::vector<RelPath> finalized;
  // A sync failed: copies may keep mtime 0 in directories finalized since,
  // so no directory is recorded as finalized any more.
  std::atomic<bool> sync_failed{false};
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

  // Whether a target file is a copy that is not final: renamed into place
  // with mtime 0 before a sync made it durable (local targets), or given
  // mtime 0 after a doubtful copy. A source file whose own mtime is 0 cannot
  // be told from one (local copies of such files are synced at once).
  bool provisional(const Entry& src, const Entry& dst) const {
    return use_mtimes && dst.mtime == Timespec{0, 0} && !same_mtime(src.mtime, dst.mtime);
  }

  // Whether the journal is kept up to date: in a run, or in a dry run that
  // verifies (--checksum), whose findings a later run repairs.
  bool journal_updates() const { return journal && (!options.dry_run || options.checksum); }

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

  // Ends the run with the error once the workers are done. The first error
  // counts.
  void end_run(Error error) {
    std::lock_guard lock(end_mutex);
    if (!end_error) end_error = std::move(error);
  }

  // A journal that cannot be read or written loses what this run and later
  // ones need: the run stops with its error.
  void journal_failed(const Error& error) {
    end_run(error);
    cancel.request();
  }

  void journal_write(const Status& written) {
    if (!written.ok()) journal_failed(written.error());
  }

  void fail(const RelPath& path, EntryType type, const Error& error) {
    if (error.kind == ErrorKind::Cancelled) return;
    Failure f{path, type, error};
    if (journal_updates()) journal_write(journal->record_failure(f));
    // Withheld deletions are recorded for --retry-failed, but reported where
    // they are withheld, and none counts as a failure in a row.
    if (error.kind == ErrorKind::Withheld) {
      report.add_failure(std::move(f), false);
      return;
    }
    report.add_failure(std::move(f));
    in_a_row(path, type, error.describe());
  }

  // An endpoint that has become unusable would fail everything that is
  // left, and fill the journal with it. A source that has gone away (an
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
    if (!journal_updates()) return;
    auto rows = prior_failures.find(parent_path(path));
    if (rows != prior_failures.end() && rows->second.count(name_of(path)))
      journal_write(journal->clear_failure(path));
  }

  // An entry that was listed on the source but is gone when it is handled:
  // the source changed, which is no failure, and the next run sees it gone.
  // A file whose copy in place (in_place) is in doubt keeps its row: the
  // source may get the file back. Many in a row, though, mean that the
  // source itself went away.
  void vanished(const RelPath& path, EntryType type, bool in_place = false) {
    stats.vanished.fetch_add(1);
    log::info(to_string(type), " ", path, " vanished from the source");
    if (!in_place || !copy_in_doubt(path)) forget_failure(path);
    in_a_row(path, type, "vanished from the source");
  }

  // Drops the journal's records of earlier failures of entries in a
  // directory, or below them, whose names are gone from both sides, which
  // leaves nothing to retry. Not where a listing left out entries: a name
  // among them is not gone.
  void clear_vanished(const std::shared_ptr<DirNode>& node, const std::vector<Entry>& src_entries,
                      const std::unordered_map<std::string, Entry>& dst_extras) {
    if (!journal_updates() || node->unlisted > 0) return;
    const RelPath& dir = node->path;
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
        if (gone(name)) journal_write(journal->clear_failure(join(dir, name)));
    for (; below != prior_failures.end() && below->first.starts_with(prefix); ++below) {
      std::string_view rest = std::string_view(below->first).substr(prefix.size());
      if (!gone(rest.substr(0, rest.find('/')))) continue;
      for (const auto& [name, kind] : below->second)
        journal_write(journal->clear_failure(join(below->first, name)));
    }
  }

  // Whether the journal records a failed copy of the file that may have left
  // it in place unverified, with the source's size and mtime, such as a
  // failed verification or close of an EOS upload that could not be taken
  // back.
  bool copy_in_doubt(const RelPath& path) const {
    auto rows = prior_failures.find(parent_path(path));
    if (rows == prior_failures.end()) return false;
    auto row = rows->second.find(name_of(path));
    return row != rows->second.end() && may_be_in_doubt(row->second);
  }

  // The metadata fields of the target entry that differ from the source.
  MetaFields differences(const Entry& src, const Entry& dst) const {
    MetaFields fields = MetaFields::None;
    bool by_size = options.size_only || options.checksum;
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
    // The mtime of a file that differs decides a copy, unless files are
    // compared otherwise.
    if (use_mtimes && (src.type != EntryType::File || by_size) && !same_mtime(src.mtime, dst.mtime))
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
    bool simulated = false;  // a dry run would have changed the metadata
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
      simulated = options.dry_run && fields != MetaFields::None;
    } else if (!cancel.requested() && !node->failed && node->made_writable) {
      // The shard that owns the directory sets its metadata, but the mode
      // relaxed for creating a subdirectory is restored here.
      if (!apply_metadata(node->path, node->source, MetaFields::Mode)) node->failed = true;
    }
    // The top directory stands for the copies that a failed sync left with
    // mtime 0, wherever they are. A recorded failure stays where a dry run
    // only would have repaired it.
    bool complete = !node->failed && !node->incomplete && !(node->path.empty() && sync_failed);
    if (complete && !simulated && !cancel.requested()) succeeded(node->path);
    if (journal && !options.dry_run && complete && !node->only && !cancel.requested()) {
      std::lock_guard lock(finalized_mutex);
      finalized.push_back(node->path);
    }
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
    if (journal && options.resume && !node->created && !node->only) {
      auto done = journal->is_done_dir(node->path);
      if (!done.ok()) journal_failed(done.error());
      if (!done.ok() || done.value()) {
        log::debug("skipping finalized directory ", node->path);
        node->owned = false;  // already finalized, leave its metadata alone
        entry_done(node);
        return;
      }
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
    // Entries that the source's listing left out are not copied.
    if (node->unlisted > 0) {
      node->incomplete = true;
      fail(node->path, EntryType::Directory,
           Error{ErrorKind::Other, std::to_string(node->unlisted) +
                                       " entries of the source could not be listed safely and "
                                       "are not copied"});
    }

    std::unordered_map<std::string, Entry> dst_entries;
    if (!node->created) {
      auto dst_listed = retry([&] { return target.list(node->path); });
      if (dst_listed.ok()) {
        node->unlisted += drop_invalid_names(target, node->path, dst_listed.value());
        node->replaces_at_open = dst_listed.value().replaces_at_open;
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
    // likely an unmounted file system or a broken server than emptied. At the
    // top, that stops a run with --delete, and a retry as well. Further down,
    // see may_delete().
    bool emptied = empty_source(src_entries) && !empty_target(dst_entries);
    if (options.delete_extra && emptied && node->path.empty()) {
      node->failed = true;
      end_run(Error{ErrorKind::Other, source.describe() + " is empty, but " + target.describe() +
                                          " is not: refusing to delete everything there"});
      entry_done(node);
      return;
    }

    for (Entry& e : src_entries) {
      if (cancel.requested()) break;
      if (node->only && !node->only->count(e.name)) continue;
      auto it = dst_entries.find(e.name);
      if (source.is_temporary(e)) {
        // Not copied, and not an extra on the target either: a user's file
        // named like one is left alone on both sides, and counted.
        stats.source_temps.fetch_add(1);
        log::debug("skipping the source's temporary ", join(node->path, e.name));
        if (it != dst_entries.end()) dst_entries.erase(it);
        continue;
      }
      const Entry* existing = it == dst_entries.end() ? nullptr : &it->second;
      switch (e.type) {
        case EntryType::Directory:
          // Neither walked nor deleted on the target.
          if (options.one_file_system && source.other_file_system(e)) {
            stats.other_fs_skipped.fetch_add(1);
            log::info("skipping ", join(node->path, e.name), ": on another file system");
          } else {
            handle_dir(node, e, existing);
          }
          break;
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
    clear_vanished(node, src_entries, dst_entries);
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
  // deletions are held back, and with --keep-emptied not in a directory
  // emptied on the source.
  bool may_delete(const std::shared_ptr<DirNode>& node, bool emptied) {
    if (!deletions_allowed(node)) return false;
    if (!emptied || !options.keep_emptied) return true;
    stats.emptied_dirs.fetch_add(1);
    log::warn("deleting nothing in ", node->path.empty() ? "." : node->path,
              ": the source's directory is empty, the target's is not (--keep-emptied)");
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

  // Whether the source lacks the extras that would be deleted, as its listing
  // says, checked by a stat of each: a listing cut short without saying so
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

  // Deletes nothing more in a directory whose listings may be incomplete, and
  // counts and logs that once. Unless the directory has a failure recorded
  // already, this records one, so that --retry-failed revisits the directory.
  void hold_deletions(const std::shared_ptr<DirNode>& node, const std::string& why) {
    if (node->deletions_held) return;
    node->deletions_held = true;
    stats.incomplete_dirs.fetch_add(1);
    log::warn("deleting nothing in ", node->path.empty() ? "." : node->path, ": ", why);
    if (!node->incomplete.exchange(true))
      fail(node->path, EntryType::Directory,
           Error{ErrorKind::Withheld, "deletions held back: " + why});
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
      // The shard that owns the directory replaces the entry. Until then,
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
    CopyJob job{node, e, existing != nullptr, std::nullopt};
    // A copy whose last attempt is in doubt, or that is not final, is made
    // again whatever a comparison would say.
    bool doubtful = existing && (copy_in_doubt(path) || provisional(e, *existing));
    if (existing && existing->size == e.size && !doubtful) {
      if (options.checksum) {
        // Stored checksums of the same type decide at once. Otherwise a
        // transfer reads the files.
        if (e.checksum.type == ChecksumType::None || e.checksum.type != existing->checksum.type) {
          job.compare = *existing;
          queue(std::move(job));
          return;
        }
        stats.checksums_compared.fetch_add(1);
        if (e.checksum == existing->checksum) {
          unchanged(node, path, e, *existing);
          return;
        }
        stats.checksums_differ.fetch_add(1);
        if (!copy_differing(job, *existing, e.checksum, existing->checksum)) return;
      } else if (options.size_only || same_mtime(existing->mtime, e.mtime)) {
        unchanged(node, path, e, *existing);
        return;
      } else if (!prepare_copy(job, existing)) {
        return;
      }
    } else {
      if (options.dry_run && options.checksum)
        differs(path, !existing ? "missing on the target"
                      : doubtful ? "the copy on the target is in doubt or not final"
                                 : std::to_string(e.size) + " bytes on the source, " +
                                       std::to_string(existing->size) + " on the target");
      if (!prepare_copy(job, existing)) return;
    }
    queue(std::move(job));
  }

  // A file that matches, whose metadata is fixed where it differs.
  void unchanged(const std::shared_ptr<DirNode>& node, const RelPath& path, const Entry& e,
                 const Entry& existing) {
    stats.files_unchanged.fetch_add(1);
    MetaFields fields = differences(e, existing);
    if (fields != MetaFields::None) ensure_writable(node);
    if (!apply_metadata(path, e, fields)) return;
    if (fields != MetaFields::None) stats.metadata_fixed.fetch_add(1);
    // A dry run that only would have fixed the metadata leaves a recorded
    // failure (a verifying one keeps its journal) for a run to repair.
    if (fields == MetaFields::None || !options.dry_run) succeeded(path);
  }

  // A file to copy where its checksum on the target differs: in a dry run a
  // failure (as --checksum verifies), else prepared for its copy. Whether it
  // is to be copied.
  bool copy_differing(const CopyJob& job, const Entry& existing, const Checksum& on_source,
                      const Checksum& on_target) {
    RelPath path = join(job.dir->path, job.source.name);
    if (options.dry_run)
      differs(path, std::string(to_string(on_source.type)) + " " + on_source.hex +
                        " on the source, " + on_target.hex + " on the target (" +
                        std::to_string(job.source.size) + " bytes)");
    return prepare_copy(job, &existing, /*differs_in_place=*/true);
  }

  // A file that differs from the source in a dry run that compares by
  // checksum: a failure, which the journal records (also in a dry run) so
  // that a run copies it again.
  void differs(const RelPath& path, const std::string& how) {
    Failure f{path, EntryType::File, Error{ErrorKind::Checksum, "differs from the source: " + how}};
    if (journal_updates()) journal_write(journal->record_failure(f));
    report.add_failure(std::move(f));
  }

  // Before a copy is queued or made: a file in place whose last copy is in
  // doubt, or that is known to differ, gets mtime 0 first unless it has it.
  // Whether the copy is to be made.
  bool prepare_copy(const CopyJob& job, const Entry* existing, bool differs_in_place = false) {
    RelPath path = join(job.dir->path, job.source.name);
    ensure_writable(job.dir);
    if (existing && (differs_in_place || copy_in_doubt(path)) &&
        !provisional(job.source, *existing) && !invalidate_in_doubt(path, job.source))
      return false;
    if (existing && job.dir->replaces_at_open && !job.dir->replacing_warned.exchange(true))
      log::warn("replacing files in ", job.dir->path.empty() ? "." : job.dir->path,
                " without atomic uploads, which the target forces to be non-atomic there: a "
                "failed replacement leaves no file");
    return true;
  }

  void queue(CopyJob job) {
    std::shared_ptr<DirNode> node = job.dir;
    node->pending.fetch_add(1);
    stats.queued_copies.fetch_add(1);
    if (!copies.push(std::move(job))) {
      stats.queued_copies.fetch_sub(1);
      entry_done(node);
    }
  }

  // Gives a file in place mtime 0 before it is copied again, as a copy that
  // is not final (see provisional()): should the new copy not arrive, every
  // later run still copies the file again, with or without a journal. One
  // that mtime 0 cannot mark, on a target without mtimes or where the
  // source has mtime 0, is removed instead, where that is safe. Whether
  // that worked.
  bool invalidate_in_doubt(const RelPath& path, const Entry& e) {
    log::debug("copying ", path, " again: its copy in place is in doubt or differs");
    if (options.dry_run) return true;
    Entry md = e;
    md.mtime = {0, 0};
    Status reset;
    if (!use_mtimes || same_mtime(e.mtime, md.mtime)) {
      if (!removes_safely) return true;
      reset = retry([&] { return target.remove(path, EntryType::File); });
      if (!reset.ok() && reset.error().kind == ErrorKind::NotFound) reset = {};
    } else {
      reset = retry([&] { return target.set_metadata(path, md, MetaFields::Mtime); });
    }
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
      if (fields != MetaFields::None) ensure_writable(node);
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
      Error withheld{ErrorKind::Withheld, "deletion cap of " + std::to_string(options.max_delete) +
                                              " reached (--max-delete), not deleting " + path};
      // Each one is a failure, but logged once.
      if (!delete_cap_reported.exchange(true))
        log::warn(withheld.describe(),
                  "; the deletions withheld after it are failures too, not logged");
      return withheld;
    }
    log::info(options.dry_run ? "would delete " : "deleting ", to_string(e.type), " ", path);
    if (!options.dry_run) {
      Status s = retry([&] { return target.remove(path, e.type); });
      if (!s.ok() && s.error().kind != ErrorKind::NotFound) {
        deletions.fetch_sub(1);
        return s;
      }
      succeeded(path);  // not in a dry run, which deletes nothing
    }
    stats.deleted.fetch_add(1);
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

  // Makes the copies so far durable and final, and records the directories
  // finalized before as such. Once that has failed, copies may keep mtime 0
  // in any directory: none is recorded any more, and the top directory is
  // a failure, so that a run that resumes or retries walks them all again.
  void sync_target() {
    std::vector<RelPath> done;
    {
      std::lock_guard lock(finalized_mutex);
      done.swap(finalized);
    }
    if (Status synced = target.flush(); !synced.ok()) {
      sync_failed = true;
      fail("", EntryType::Directory, synced.error());
      return;
    }
    if (sync_failed) return;
    for (const RelPath& path : done) journal_write(journal->record_done_dir(path));
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

  // Compares a file of the source's size on the target by checksum: by the
  // ones stored on both sides where they are of the same type, else by the
  // type that the target stores (or the source, or adler32), computed by
  // reading the files that store none of it. Whether it differs and is to be
  // copied. One that matches is unchanged.
  bool differs_by_checksum(const CopyJob& job, BufferPool& pool) {
    RelPath path = join(job.dir->path, job.source.name);
    const Entry& existing = *job.compare;
    auto on_source = retry([&] { return source.stored_checksum(path, job.source); });
    if (!on_source.ok()) return compare_failed(path, on_source.error(), true);
    auto on_target = retry([&] { return target.stored_checksum(path, existing); });
    if (!on_target.ok()) return compare_failed(path, on_target.error(), false);
    ChecksumType type = on_target.value().type;
    if (type == ChecksumType::None) type = on_source.value().type;
    if (type == ChecksumType::None) type = ChecksumType::Adler32;
    auto of = [&](Endpoint& ep, const Entry& listed, const Checksum& stored) -> Result<Checksum> {
      if (stored.type == type) return stored;
      return retry([&] { return read_checksum(ep, path, listed, type, pool, cancel); });
    };
    auto s = of(source, job.source, on_source.value());
    if (!s.ok()) return compare_failed(path, s.error(), true);
    auto d = of(target, existing, on_target.value());
    if (!d.ok()) return compare_failed(path, d.error(), false);
    stats.checksums_compared.fetch_add(1);
    if (s.value() == d.value()) {
      unchanged(job.dir, path, job.source, existing);
      return false;
    }
    stats.checksums_differ.fetch_add(1);
    return copy_differing(job, existing, s.value(), d.value());
  }

  // A comparison that failed. A source file that is gone has vanished, a
  // target file that is gone is copied anew, and other errors are failures.
  // Whether the file is to be copied.
  bool compare_failed(const RelPath& path, const Error& error, bool on_source) {
    if (error.kind != ErrorKind::NotFound) {
      fail(path, EntryType::File, error);
      return false;
    }
    if (on_source) {
      vanished(path, EntryType::File, /*in_place=*/true);
      return false;
    }
    if (options.dry_run) differs(path, "missing on the target");
    return true;
  }

  void run_copy(const CopyJob& job, BufferPool& pool, TransferSlot& slot) {
    if (job.compare && (cancel.requested() || !differs_by_checksum(job, pool))) return;
    RelPath path = join(job.dir->path, job.source.name);
    job.dir->touched = true;
    bool non_atomic = job.replaces && job.dir->replaces_at_open;
    if (options.dry_run) {
      stats.files_copied.fetch_add(1);
      stats.bytes_copied.fetch_add(job.source.size);
      if (non_atomic) stats.non_atomic_replacements.fetch_add(1);
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
      // A copy that is in place before it is confirmed is first journaled as
      // in doubt. A run that ends in between leaves that row for the next.
      bool pending = false;
      if (journal && confirms_after_commit)
        job_options.before_commit = [&] {
          if (pending) return Status();
          pending = true;
          Status recorded = journal->record_failure(
              {path, EntryType::File,
               Error{ErrorKind::IO, "the run ended before the copy was confirmed"}});
          journal_write(recorded);
          return recorded;
        };
      // The pending row goes once the copy is settled. A failure replaces it.
      auto settled = [&] {
        if (pending) journal_write(journal->clear_failure(path));
      };
      // The error of an attempt that may have left its copy in place. A
      // copy in doubt since an earlier run stays so until one is confirmed,
      // whatever the attempts of this run fail with.
      std::optional<Error> doubt;
      bool prior_doubt = copy_in_doubt(path);
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
        if (non_atomic) stats.non_atomic_replacements.fetch_add(1);
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
        settled();
      } else if (result.error().kind == ErrorKind::NotFound && !target_failed && !doubt) {
        vanished(path, EntryType::File, job.replaces);  // keeps a row in doubt
        settled();
      } else if (doubt || prior_doubt) {
        // The journal keeps the copy in doubt, however its last attempt
        // ended, also when cancelled (which fail() leaves out, so that the
        // row of the earlier run stays as it is).
        Error e = result.error();
        if (e.kind == ErrorKind::Cancelled && doubt)
          e = *doubt;
        else if (!e.in_doubt && e.kind != ErrorKind::Cancelled)
          e = Error{ErrorKind::IO, e.describe() + "; " +
                                       (doubt ? "an attempt before: " + doubt->describe()
                                              : "the copy in place is in doubt since an earlier run")};
        fail(path, EntryType::File, e);
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
    if (!target.capabilities().can_set_mtime && !options.size_only && !options.checksum)
      return Error{ErrorKind::Unsupported, target.describe() +
                                               " stores no mtimes, so that files can be compared "
                                               "by size only: use --size-only (or --checksum)"};
    // Deleting through a symlink there deletes what it points to.
    if (options.delete_extra && target.capabilities().follows_symlinks &&
        !options.target_without_symlinks)
      return Error{ErrorKind::Unsupported,
                   target.describe() + " cannot show symlinks, and deletes what they point to: " +
                       "--delete there needs --target-without-symlinks, which states that it has "
                       "none"};
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
    confirms_after_commit = dst.confirms_after_commit;
    if (options.preserve_mode && !dst.can_set_mode) {
      log::warn(target.describe(), " sets no modes: modes are not synchronized");
      options.preserve_mode = false;
      copy_options.preserve_mode = false;
    }
    if (options.preserve_mode && (file_mode_bits != 07777 || dir_mode_bits != 07777)) {
      char bits[80];
      std::snprintf(bits, sizeof bits, "%04o of files and %04o of directories", file_mode_bits,
                    dir_mode_bits);
      log::info(target.describe(), " stores only the mode bits ", bits);
    }
    relax_modes = !dst.can_set_owner && options.preserve_mode;
    removes_safely = !dst.follows_symlinks || options.target_without_symlinks;
    copy_options.preserve_mtime = use_mtimes;
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

  // Whether every directory above a path is a directory on an endpoint, as
  // a run that starts below the top takes for granted: a walk from the top
  // would replace a symlink or file there, a run below it would follow it.
  Status ancestors_are_directories(Endpoint& ep, const RelPath& path) {
    for (auto slash = path.find('/'); slash != std::string::npos; slash = path.find('/', slash + 1)) {
      RelPath ancestor = path.substr(0, slash);
      auto st = retry([&] { return ep.stat(ancestor); });
      if (!st.ok()) return st.error();
      if (st.value().type != EntryType::Directory)
        return Error{ErrorKind::NotADirectory, ep.describe() + "/" + ancestor + " is a " +
                                                   std::string(to_string(st.value().type)) +
                                                   ", not a directory"};
    }
    return {};
  }

  // The entries of a directory that is gone from the source, or is no
  // directory there any more, are not retried. The rows of those also gone
  // from the target are dropped, since nothing is left to retry. The others
  // stay until a run removes the entries or sees them gone: a copy among
  // them may be in doubt.
  void retry_without_source(const RelPath& dir, const std::set<std::string>& names) {
    for (const auto& name : names) {
      if (cancel.requested()) return;
      RelPath path = join(dir, name);
      auto dst = retry([&] { return target.stat(path); });
      if (!dst.ok() && (dst.error().kind == ErrorKind::NotFound ||
                        dst.error().kind == ErrorKind::NotADirectory)) {
        if (journal_updates()) journal_write(journal->clear_failure(path));
        continue;
      }
      EntryType type = dst.ok() ? dst.value().type : EntryType::File;
      report.add_failure(
          {path, type,
           Error{ErrorKind::NotFound, "gone from the source while the target has it: not "
                                      "retried (a run with --delete removes it)"}});
    }
  }

  // With --one-file-system: an error if the source directory, or one above
  // it, is on another file system than the source's root, where a walk from
  // the top would not go.
  Status on_root_file_system(const RelPath& dir, const Entry& entry) {
    auto check = [&](const RelPath& path, const Entry& e) {
      if (!source.other_file_system(e)) return Status();
      return Status(Error{ErrorKind::Other, source.describe() + "/" + path +
                                                " is on another file system (--one-file-system)"});
    };
    for (auto slash = dir.find('/'); slash != std::string::npos; slash = dir.find('/', slash + 1)) {
      RelPath ancestor = dir.substr(0, slash);
      auto st = retry([&] { return source.stat(ancestor); });
      if (!st.ok()) return st.error();
      if (Status s = check(ancestor, st.value()); !s.ok()) return s;
    }
    return check(dir, entry);
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
    if (journal) {
      auto failures = journal->failures();
      if (!failures.ok()) return failures.error();
      for (const Failure& f : failures.value())
        prior_failures[parent_path(f.path)][name_of(f.path)] = f.error.kind;
    }
    adapt_to_capabilities();
    {
      std::lock_guard lock(done_mutex);
      roots_pending = roots.size();
    }
    size_t transfers = max_transfers();
    // If a thread cannot be started, the run ends before it begins, and so do
    // the threads started so far.
    bool started = false;
    try {
      for (int i = 0; i < checkers(); ++i) threads.emplace_back([this] { checker_loop(); });
      for (size_t i = 0; i < transfers; ++i)
        threads.emplace_back([this, i] { transfer_loop(i); });
      if (initial_limit() < transfers) controller = std::thread([this] { controller_loop(); });
      if (!options.dry_run)
        syncer = std::thread([this] {
          // By the system clock: ThreadSanitizer up to GCC 11 misses the
          // waits on the steady one (pthread_cond_clockwait).
          std::unique_lock lock(syncer_mutex);
          while (!syncer_cv.wait_until(lock,
                                       std::chrono::system_clock::now() + options.sync_interval,
                                       [&] { return syncer_stop; })) {
            lock.unlock();
            sync_target();
            lock.lock();
          }
        });
      started = true;
    } catch (const std::system_error& e) {
      end_run(errno_error(e.code().value(), "start the threads of the run"));
      cancel.request();
    }
    if (started)
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
    {
      std::lock_guard lock(syncer_mutex);
      syncer_stop = true;
    }
    syncer_cv.notify_all();
    if (syncer.joinable()) syncer.join();
    // Also after a cancellation: the copies made so far stay.
    if (!options.dry_run) sync_target();
    // The top directory's finalization may have cleared the failure that a
    // sync recorded meanwhile: recorded again, once nothing else runs.
    if (sync_failed && journal_updates())
      journal_write(journal->record_failure(
          {"", EntryType::Directory,
           Error{ErrorKind::IO, "a sync of the target failed: copies may keep mtime 0"}}));
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

  // Entries below a retried entry are covered by its walk, whatever it was
  // recorded as: a directory is walked whole, also one that was a file.
  std::set<RelPath> retried;
  for (const Failure& f : entries) retried.insert(f.path);

  // The rest is grouped by parent directory. Each group becomes a node that
  // handles only those names, recursing into directories among them.
  std::map<RelPath, std::set<std::string>> groups;
  bool whole_tree = false;
  for (const Failure& f : entries) {
    bool covered = false;
    for (RelPath dir = f.path; !dir.empty() && !covered;) {
      dir = parent_path(dir);
      covered = retried.count(dir) > 0;
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
    if (impl_->cancel.requested()) break;
    auto src = impl_->source_dir(dir);
    if (!src.ok() && (src.error().kind == ErrorKind::NotFound ||
                      src.error().kind == ErrorKind::NotADirectory)) {
      impl_->retry_without_source(dir, names);
      continue;
    }
    // Where a walk from the top would not go, nor does a retry: the rows
    // stay for a run that does.
    if (src.ok() && impl_->options.one_file_system) {
      Status same = impl_->on_root_file_system(dir, src.value());
      if (!same.ok()) {
        if (same.error().kind != ErrorKind::Other) {
          impl_->fail(dir, EntryType::Directory, same.error());
          continue;
        }
        impl_->stats.other_fs_skipped.fetch_add(1);
        log::info("skipping ", dir, ": ", same.error().message, "; its entries stay recorded");
        continue;
      }
    }
    // Starting below the top takes the paths above for directories.
    Status above = src.ok() ? impl_->ancestors_are_directories(impl_->source, dir) : Status();
    if (above.ok()) above = impl_->ancestors_are_directories(impl_->target, dir);
    auto node = !above.ok() ? Result<std::shared_ptr<DirNode>>(above.error())
                : src.ok()  ? impl_->make_node(dir, src.value())
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
