// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "eosmirror/error.hh"
#include "eosmirror/report.hh"
#include "eosmirror/types.hh"

struct sqlite3;
struct sqlite3_stmt;

namespace eosmirror {

// The SQLite journal of a source/target pair: the entries that failed after
// retries, and the directories finalized by the current run (for resuming).
// All methods are thread-safe.
class Journal {
 public:
  // Opens or creates the journal. A journal belongs to one source, target
  // and shard; opening it for another combination fails.
  static Result<std::unique_ptr<Journal>> open(const std::string& file, const std::string& source,
                                               const std::string& target,
                                               const std::string& shard = "0/1");

  // Opens an existing journal for inspection, whatever pair it belongs to.
  static Result<std::unique_ptr<Journal>> open_any(const std::string& file);

  std::string source();
  std::string target();
  ~Journal();
  Journal(const Journal&) = delete;
  Journal& operator=(const Journal&) = delete;

  // Starts a run. Finalized directories of earlier runs are forgotten,
  // unless resuming a run that did not complete.
  Status begin_run(bool resume);
  Status end_run(bool completed);
  int64_t run_id() const { return run_id_; }

  // Records a failure, replacing an earlier one for the same path.
  void record_failure(const Failure& failure);
  void clear_failure(const RelPath& path);
  std::vector<Failure> failures();

  void record_done_dir(const RelPath& path);
  bool is_done_dir(const RelPath& path);

 private:
  explicit Journal(sqlite3* db) : db_(db) {}
  static Result<std::unique_ptr<Journal>> open_db(const std::string& file, bool create);
  Status exec(const std::string& sql);
  Result<std::string> meta(const std::string& key);
  Status set_meta(const std::string& key, const std::string& value);
  Status prepare_statements();
  Error db_error(const std::string& context);

  sqlite3* db_;
  std::mutex mutex_;
  int64_t run_id_ = 0;
  sqlite3_stmt* insert_failure_ = nullptr;
  sqlite3_stmt* delete_failure_ = nullptr;
  sqlite3_stmt* select_failures_ = nullptr;
  sqlite3_stmt* insert_done_ = nullptr;
  sqlite3_stmt* select_done_ = nullptr;
};

}  // namespace eosmirror
