// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "eosmirror/error.hh"
#include "eosmirror/report.hh"
#include "eosmirror/types.hh"

struct sqlite3;
struct sqlite3_stmt;

namespace eosmirror {

// The options of a run that decide what it makes of the target, as names
// and values, which a resumed run must share with the run it resumes.
using RunOptions = std::vector<std::pair<std::string, std::string>>;

// The SQLite journal of a source/target pair: the entries that failed after
// retries, and the directories finalized by the current run (for resuming).
// All methods are thread-safe. A journal that cannot be read or written
// says so in errors, which a run cannot go on without.
class Journal {
 public:
  // Opens or creates the journal. A journal belongs to one source, target
  // and shard; opening it for another combination fails, and so does
  // opening a database that is not a journal. Checking the owner and
  // recording that of a new journal is one transaction, so that runs
  // started together cannot both take a new journal.
  static Result<std::unique_ptr<Journal>> open(const std::string& file, const std::string& source,
                                               const std::string& target,
                                               const std::string& shard = "0/1");

  // Opens an existing journal read-only, changing nothing (nor making the
  // files of a write-ahead log): for a dry run, which names its source,
  // target and shard as for open(), or for inspection, which names none. An
  // error for a file that is not a journal.
  static Result<std::unique_ptr<Journal>> open_read_only(const std::string& file,
                                                         const std::string& source = "",
                                                         const std::string& target = "",
                                                         const std::string& shard = "0/1");

  std::string source();
  std::string target();
  ~Journal();
  Journal(const Journal&) = delete;
  Journal& operator=(const Journal&) = delete;

  // Whether the last run was interrupted, which leaves it to resume.
  Result<bool> interrupted();
  // The names of the given options that the last run had otherwise; an
  // option it has no record of counts as the same.
  Result<std::vector<std::string>> differing_options(const RunOptions& options);

  // Starts a run and records its options. Finalized directories of earlier
  // runs are forgotten, unless resuming a run that was interrupted.
  Status begin_run(bool resume, const RunOptions& options = {});
  Status end_run(bool completed);
  int64_t run_id() const { return run_id_; }

  // Records a failure, replacing an earlier one for the same path.
  Status record_failure(const Failure& failure);
  Status clear_failure(const RelPath& path);
  Result<std::vector<Failure>> failures();

  Status record_done_dir(const RelPath& path);
  Result<bool> is_done_dir(const RelPath& path);

 private:
  Journal(sqlite3* db, std::string file) : db_(db), file_(std::move(file)) {}
  Status exec(const std::string& sql);
  Result<std::string> meta(const std::string& key);
  Status set_meta(const std::string& key, const std::string& value);
  Status claim(const std::string& source, const std::string& target, const std::string& shard);
  Status check_owner(const std::string& source, const std::string& target,
                     const std::string& shard);
  Result<bool> last_run_interrupted();
  Status prepare_statements();
  Error db_error(const std::string& context);
  Error not_a_journal(const std::string& why) const;

  sqlite3* db_;
  std::string file_;
  std::mutex mutex_;
  int64_t run_id_ = 0;
  sqlite3_stmt* insert_failure_ = nullptr;
  sqlite3_stmt* delete_failure_ = nullptr;
  sqlite3_stmt* select_failures_ = nullptr;
  sqlite3_stmt* insert_done_ = nullptr;
  sqlite3_stmt* select_done_ = nullptr;
};

}  // namespace eosmirror
