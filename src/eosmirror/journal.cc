// SPDX-License-Identifier: GPL-3.0-or-later
#include "eosmirror/journal.hh"

#include <fcntl.h>
#include <sqlite3.h>
#include <sys/file.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string_view>

namespace eosmirror {

namespace {

constexpr const char* kSchema = R"(
CREATE TABLE IF NOT EXISTS meta (key TEXT PRIMARY KEY, value TEXT NOT NULL);
CREATE TABLE IF NOT EXISTS runs (
  id INTEGER PRIMARY KEY, started INTEGER NOT NULL, finished INTEGER, completed INTEGER);
CREATE TABLE IF NOT EXISTS failures (
  path TEXT PRIMARY KEY, type INTEGER NOT NULL, kind INTEGER NOT NULL, message TEXT NOT NULL,
  run_id INTEGER NOT NULL, updated INTEGER NOT NULL);
CREATE TABLE IF NOT EXISTS done_dirs (path TEXT PRIMARY KEY, run_id INTEGER NOT NULL);
)";

// The prefix of the meta rows that hold the options of the last run.
constexpr std::string_view kOptionKey = "option ";

int64_t now_unix() {
  return std::chrono::duration_cast<std::chrono::seconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

void bind_text(sqlite3_stmt* stmt, int index, const std::string& text) {
  sqlite3_bind_text(stmt, index, text.data(), static_cast<int>(text.size()), SQLITE_TRANSIENT);
}

// Runs a prepared statement that returns no rows to completion and resets it.
int step_done(sqlite3_stmt* stmt) {
  int rc = sqlite3_step(stmt);
  sqlite3_reset(stmt);
  sqlite3_clear_bindings(stmt);
  return rc == SQLITE_DONE ? SQLITE_OK : rc;
}

// The URI that opens a file read-only. A journal without a write-ahead log
// is opened as immutable, since nobody writes a journal without a log. That
// keeps SQLite from making the log and its index, as it would for a reader
// of a journal in WAL mode, and leaving them behind.
std::string read_only_uri(const std::string& file) {
  std::error_code ec;
  bool logged = std::filesystem::exists(file + "-wal", ec);
  std::string uri = file.starts_with('/') ? "file://" : "file:";
  for (char c : file) {
    if (c == '%' || c == '?' || c == '#') {
      char escaped[4];
      std::snprintf(escaped, sizeof escaped, "%%%02X", static_cast<unsigned char>(c));
      uri += escaped;
    } else {
      uri += c;
    }
  }
  return uri + (logged ? "?mode=ro" : "?mode=ro&immutable=1");
}

}  // namespace

Result<std::unique_ptr<Journal>> Journal::open(const std::string& file, const std::string& source,
                                               const std::string& target,
                                               const std::string& shard) {
  sqlite3* db = nullptr;
  int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_NOMUTEX;
  if (sqlite3_open_v2(file.c_str(), &db, flags, nullptr) != SQLITE_OK) {
    Error e{ErrorKind::Other, "open journal " + file + ": " + (db ? sqlite3_errmsg(db) : "error")};
    sqlite3_close(db);
    return e;
  }
  std::unique_ptr<Journal> journal(new Journal(db, file));
  if (Status s = journal->lock(); !s.ok()) return s.error();
  sqlite3_busy_timeout(db, 10000);
  if (Status s = journal->claim(source, target, shard); !s.ok()) return s.error();
  // Outside the transaction, which the journal mode cannot change in.
  for (const char* pragma : {"PRAGMA journal_mode=WAL", "PRAGMA synchronous=NORMAL"})
    if (Status s = journal->exec(pragma); !s.ok()) return s.error();
  if (Status s = journal->prepare_statements(); !s.ok()) return s.error();
  return journal;
}

Result<std::unique_ptr<Journal>> Journal::open_read_only(const std::string& file,
                                                         const std::string& source,
                                                         const std::string& target,
                                                         const std::string& shard) {
  sqlite3* db = nullptr;
  int flags = SQLITE_OPEN_READONLY | SQLITE_OPEN_URI | SQLITE_OPEN_NOMUTEX;
  if (sqlite3_open_v2(read_only_uri(file).c_str(), &db, flags, nullptr) != SQLITE_OK) {
    Error e{ErrorKind::Other, "open journal " + file + ": " + (db ? sqlite3_errmsg(db) : "error")};
    sqlite3_close(db);
    return e;
  }
  std::unique_ptr<Journal> journal(new Journal(db, file));
  if (!source.empty()) {
    if (Status s = journal->lock(); !s.ok()) return s.error();
  }
  sqlite3_busy_timeout(db, 10000);
  if (Status s = journal->prepare_statements(); !s.ok())
    return journal->not_a_journal(s.error().message);
  for (const char* key : {"source", "target"}) {
    auto value = journal->meta(key);
    if (!value.ok() && value.error().kind == ErrorKind::NotFound)
      return journal->not_a_journal("no " + std::string(key));
    if (!value.ok()) return value.error();
  }
  if (!source.empty()) {
    if (Status s = journal->check_owner(source, target, shard); !s.ok()) return s.error();
  }
  return journal;
}

// Takes the journal's lock for this object's life: flock on the database
// file, which SQLite's own (fcntl) locks do not interfere with.
Status Journal::lock() {
  lock_fd_ = ::open(file_.c_str(), O_RDONLY | O_CLOEXEC);
  if (lock_fd_ < 0) return errno_error(errno, "open journal " + file_);
  if (flock(lock_fd_, LOCK_EX | LOCK_NB) == 0) return {};
  if (errno == EWOULDBLOCK)
    return Error{ErrorKind::Other, "journal " + file_ + " is in use by another run"};
  return errno_error(errno, "lock journal " + file_);
}

// Takes a new journal for source, target and shard, or checks that an
// existing one is theirs, in one transaction.
Status Journal::claim(const std::string& source, const std::string& target,
                      const std::string& shard) {
  if (Status s = exec("BEGIN IMMEDIATE"); !s.ok()) return not_a_journal(s.error().message);
  Status s = [&]() -> Status {
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, "SELECT count(*) FROM sqlite_master WHERE type = 'table'", -1,
                           &stmt, nullptr) != SQLITE_OK)
      return not_a_journal(sqlite3_errmsg(db_));
    int rc = sqlite3_step(stmt);
    bool empty = rc == SQLITE_ROW && sqlite3_column_int(stmt, 0) == 0;
    sqlite3_finalize(stmt);
    if (rc != SQLITE_ROW) return not_a_journal(sqlite3_errmsg(db_));
    // A journal records its owner when it is made: a database with tables
    // and no owner is someone else's.
    if (!empty) {
      auto existing = meta("source");
      if (!existing.ok())
        return not_a_journal(existing.error().kind == ErrorKind::NotFound
                                 ? "no source"
                                 : existing.error().message);
    }
    if (Status created = exec(kSchema); !created.ok()) return created;
    if (!empty) return check_owner(source, target, shard);
    for (const auto& [key, value] : {std::pair{"source", source}, std::pair{"target", target},
                                     std::pair{"shard", shard}})
      if (Status set = set_meta(key, value); !set.ok()) return set;
    return {};
  }();
  if (!s.ok()) {
    Status rolled_back = exec("ROLLBACK");
    (void)rolled_back;
    return s;
  }
  return exec("COMMIT");
}

// An error unless the journal belongs to source, target and shard.
Status Journal::check_owner(const std::string& source, const std::string& target,
                            const std::string& shard) {
  auto existing_source = meta("source");
  auto existing_target = meta("target");
  auto existing_shard = meta("shard");
  for (const auto* m : {&existing_source, &existing_target, &existing_shard})
    if (!m->ok() && m->error().kind != ErrorKind::NotFound) return m->error();
  if (!existing_source.ok() || !existing_target.ok()) return not_a_journal("no source or target");
  std::string their_shard = existing_shard.ok() ? existing_shard.value() : "0/1";
  if (existing_source.value() == source && existing_target.value() == target &&
      their_shard == shard)
    return {};
  std::string described = existing_source.value() + " -> " + existing_target.value();
  if (their_shard != "0/1") described += " (shard " + their_shard + ")";
  return Error{ErrorKind::Other, "journal " + file_ + " belongs to " + described + ", not to " +
                                     source + " -> " + target +
                                     (shard != "0/1" ? " (shard " + shard + ")" : "")};
}

std::string Journal::source() {
  auto m = meta("source");
  return m.ok() ? m.value() : "";
}

std::string Journal::target() {
  auto m = meta("target");
  return m.ok() ? m.value() : "";
}

Journal::~Journal() {
  for (sqlite3_stmt* stmt :
       {insert_failure_, delete_failure_, select_failures_, insert_done_, select_done_})
    sqlite3_finalize(stmt);
  sqlite3_close(db_);
  // After the database: closing a descriptor of the file drops the fcntl
  // locks that SQLite holds on it.
  if (lock_fd_ >= 0) close(lock_fd_);
}

Error Journal::db_error(const std::string& context) {
  return Error{ErrorKind::Other, "journal " + file_ + ": " + context + ": " + sqlite3_errmsg(db_)};
}

Error Journal::not_a_journal(const std::string& why) const {
  return Error{ErrorKind::Other, file_ + " is not an eosmirror journal (" + why + ")"};
}

Status Journal::exec(const std::string& sql) {
  char* msg = nullptr;
  if (sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, &msg) != SQLITE_OK) {
    Error e{ErrorKind::Other, "journal " + file_ + ": " + (msg ? msg : "error")};
    sqlite3_free(msg);
    return e;
  }
  return {};
}

Result<std::string> Journal::meta(const std::string& key) {
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db_, "SELECT value FROM meta WHERE key = ?", -1, &stmt, nullptr) !=
      SQLITE_OK)
    return db_error("read meta");
  bind_text(stmt, 1, key);
  Result<std::string> result = Error{ErrorKind::NotFound, "no meta " + key};
  int rc = sqlite3_step(stmt);
  if (rc == SQLITE_ROW)
    result = std::string(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0)));
  else if (rc != SQLITE_DONE)
    result = db_error("read meta");
  sqlite3_finalize(stmt);
  return result;
}

Status Journal::set_meta(const std::string& key, const std::string& value) {
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db_, "INSERT OR REPLACE INTO meta (key, value) VALUES (?, ?)", -1, &stmt,
                         nullptr) != SQLITE_OK)
    return db_error("write meta");
  bind_text(stmt, 1, key);
  bind_text(stmt, 2, value);
  int rc = step_done(stmt);
  sqlite3_finalize(stmt);
  if (rc != SQLITE_OK) return db_error("write meta");
  return {};
}

Status Journal::prepare_statements() {
  struct {
    sqlite3_stmt** stmt;
    const char* sql;
  } statements[] = {
      {&insert_failure_,
       "INSERT OR REPLACE INTO failures (path, type, kind, message, run_id, updated) "
       "VALUES (?, ?, ?, ?, ?, ?)"},
      {&delete_failure_, "DELETE FROM failures WHERE path = ?"},
      {&select_failures_, "SELECT path, type, kind, message FROM failures ORDER BY path"},
      {&insert_done_, "INSERT OR REPLACE INTO done_dirs (path, run_id) VALUES (?, ?)"},
      {&select_done_, "SELECT 1 FROM done_dirs WHERE path = ?"},
  };
  for (auto& s : statements)
    if (sqlite3_prepare_v2(db_, s.sql, -1, s.stmt, nullptr) != SQLITE_OK)
      return db_error("prepare statement");
  return {};
}

Result<bool> Journal::interrupted() {
  std::lock_guard lock(mutex_);
  return last_run_interrupted();
}

Result<bool> Journal::last_run_interrupted() {
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db_, "SELECT completed FROM runs ORDER BY id DESC LIMIT 1", -1, &stmt,
                         nullptr) != SQLITE_OK)
    return db_error("read runs");
  int rc = sqlite3_step(stmt);
  bool interrupted = rc == SQLITE_ROW && (sqlite3_column_type(stmt, 0) == SQLITE_NULL ||
                                          sqlite3_column_int(stmt, 0) == 0);
  sqlite3_finalize(stmt);
  if (rc != SQLITE_ROW && rc != SQLITE_DONE) return db_error("read runs");
  return interrupted;
}

Result<std::vector<std::string>> Journal::differing_options(const RunOptions& options) {
  std::lock_guard lock(mutex_);
  std::vector<std::string> differing;
  for (const auto& [name, value] : options) {
    auto recorded = meta(std::string(kOptionKey) + name);
    if (!recorded.ok() && recorded.error().kind != ErrorKind::NotFound) return recorded.error();
    bool off = value == "no" || value.empty();
    if (recorded.ok() ? recorded.value() != value : !off) differing.push_back(name);
  }
  return differing;
}

Status Journal::begin_run(bool resume, const RunOptions& options) {
  std::lock_guard lock(mutex_);
  if (Status s = exec("BEGIN IMMEDIATE"); !s.ok()) return s;
  Status s = [&]() -> Status {
    auto interrupted = last_run_interrupted();
    if (!interrupted.ok()) return interrupted.error();
    if (!resume || !interrupted.value()) {
      if (Status deleted = exec("DELETE FROM done_dirs"); !deleted.ok()) return deleted;
    }
    for (const auto& [name, value] : options)
      if (Status set = set_meta(std::string(kOptionKey) + name, value); !set.ok()) return set;
    return exec("INSERT INTO runs (started) VALUES (" + std::to_string(now_unix()) + ")");
  }();
  if (!s.ok()) {
    Status rolled_back = exec("ROLLBACK");
    (void)rolled_back;
    return s;
  }
  run_id_ = sqlite3_last_insert_rowid(db_);
  return exec("COMMIT");
}

Status Journal::end_run(bool completed) {
  std::lock_guard lock(mutex_);
  return exec("UPDATE runs SET finished = " + std::to_string(now_unix()) +
              ", completed = " + (completed ? "1" : "0") + " WHERE id = " +
              std::to_string(run_id_));
}

Status Journal::record_failure(const Failure& failure) {
  std::lock_guard lock(mutex_);
  bind_text(insert_failure_, 1, failure.path);
  sqlite3_bind_int(insert_failure_, 2, static_cast<int>(failure.type));
  sqlite3_bind_int(insert_failure_, 3, static_cast<int>(failure.error.kind));
  bind_text(insert_failure_, 4, failure.error.describe());
  sqlite3_bind_int64(insert_failure_, 5, run_id_);
  sqlite3_bind_int64(insert_failure_, 6, now_unix());
  if (step_done(insert_failure_) != SQLITE_OK) return db_error("record failure of " + failure.path);
  return {};
}

Status Journal::clear_failure(const RelPath& path) {
  std::lock_guard lock(mutex_);
  bind_text(delete_failure_, 1, path);
  if (step_done(delete_failure_) != SQLITE_OK) return db_error("clear failure of " + path);
  return {};
}

Result<std::vector<Failure>> Journal::failures() {
  std::lock_guard lock(mutex_);
  std::vector<Failure> result;
  int rc;
  while ((rc = sqlite3_step(select_failures_)) == SQLITE_ROW) {
    Failure f;
    f.path = reinterpret_cast<const char*>(sqlite3_column_text(select_failures_, 0));
    f.type = static_cast<EntryType>(sqlite3_column_int(select_failures_, 1));
    f.error.kind = static_cast<ErrorKind>(sqlite3_column_int(select_failures_, 2));
    f.error.message = reinterpret_cast<const char*>(sqlite3_column_text(select_failures_, 3));
    result.push_back(std::move(f));
  }
  sqlite3_reset(select_failures_);
  if (rc != SQLITE_DONE) return db_error("read failures");
  return result;
}

Status Journal::record_done_dir(const RelPath& path) {
  std::lock_guard lock(mutex_);
  bind_text(insert_done_, 1, path);
  sqlite3_bind_int64(insert_done_, 2, run_id_);
  if (step_done(insert_done_) != SQLITE_OK) return db_error("record directory " + path);
  return {};
}

Result<bool> Journal::is_done_dir(const RelPath& path) {
  std::lock_guard lock(mutex_);
  bind_text(select_done_, 1, path);
  int rc = sqlite3_step(select_done_);
  sqlite3_reset(select_done_);
  sqlite3_clear_bindings(select_done_);
  if (rc != SQLITE_ROW && rc != SQLITE_DONE) return db_error("read directory " + path);
  return rc == SQLITE_ROW;
}

}  // namespace eosmirror
