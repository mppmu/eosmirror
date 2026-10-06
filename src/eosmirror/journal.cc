// SPDX-License-Identifier: GPL-3.0-or-later
#include "eosmirror/journal.hh"

#include <sqlite3.h>

#include <chrono>

#include "eosmirror/log.hh"

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

int64_t now_unix() {
  return std::chrono::duration_cast<std::chrono::seconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

void bind_text(sqlite3_stmt* stmt, int index, const std::string& text) {
  sqlite3_bind_text(stmt, index, text.data(), static_cast<int>(text.size()), SQLITE_TRANSIENT);
}

// Runs a prepared statement to completion and resets it.
int step_done(sqlite3_stmt* stmt) {
  int rc = sqlite3_step(stmt);
  sqlite3_reset(stmt);
  sqlite3_clear_bindings(stmt);
  return rc == SQLITE_DONE || rc == SQLITE_ROW ? SQLITE_OK : rc;
}

}  // namespace

Result<std::unique_ptr<Journal>> Journal::open_db(const std::string& file, bool create) {
  sqlite3* db = nullptr;
  int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_NOMUTEX | (create ? SQLITE_OPEN_CREATE : 0);
  int rc = sqlite3_open_v2(file.c_str(), &db, flags, nullptr);
  if (rc != SQLITE_OK) {
    Error e{ErrorKind::Other, "open journal " + file + ": " + (db ? sqlite3_errmsg(db) : "error")};
    sqlite3_close(db);
    return e;
  }
  std::unique_ptr<Journal> journal(new Journal(db));
  sqlite3_busy_timeout(db, 10000);
  for (const char* pragma : {"PRAGMA journal_mode=WAL", "PRAGMA synchronous=NORMAL"}) {
    Status s = journal->exec(pragma);
    if (!s.ok()) return s.error();
  }
  Status s = journal->exec(kSchema);
  if (!s.ok()) return s.error();
  if (!(s = journal->prepare_statements()).ok()) return s.error();
  return journal;
}

Result<std::unique_ptr<Journal>> Journal::open_any(const std::string& file) {
  return open_db(file, /*create=*/false);
}

std::string Journal::source() {
  auto m = meta("source");
  return m.ok() ? m.value() : "";
}

std::string Journal::target() {
  auto m = meta("target");
  return m.ok() ? m.value() : "";
}

Result<std::unique_ptr<Journal>> Journal::open(const std::string& file, const std::string& source,
                                               const std::string& target) {
  auto opened = open_db(file, /*create=*/true);
  if (!opened.ok()) return opened.error();
  std::unique_ptr<Journal> journal = std::move(opened).value();
  Status s;

  auto existing_source = journal->meta("source");
  auto existing_target = journal->meta("target");
  if (!existing_source.ok() || !existing_target.ok()) {
    if (!(s = journal->set_meta("source", source)).ok()) return s.error();
    if (!(s = journal->set_meta("target", target)).ok()) return s.error();
  } else if (existing_source.value() != source || existing_target.value() != target) {
    return Error{ErrorKind::Other, "journal " + file + " belongs to " + existing_source.value() +
                                       " -> " + existing_target.value() + ", not to " + source +
                                       " -> " + target};
  }
  return journal;
}

Journal::~Journal() {
  for (sqlite3_stmt* stmt :
       {insert_failure_, delete_failure_, select_failures_, insert_done_, select_done_})
    sqlite3_finalize(stmt);
  sqlite3_close(db_);
}

Error Journal::db_error(const std::string& context) {
  return Error{ErrorKind::Other, "journal: " + context + ": " + sqlite3_errmsg(db_)};
}

Status Journal::exec(const std::string& sql) {
  char* msg = nullptr;
  if (sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, &msg) != SQLITE_OK) {
    Error e{ErrorKind::Other, std::string("journal: ") + (msg ? msg : "error")};
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
  if (sqlite3_step(stmt) == SQLITE_ROW)
    result = std::string(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0)));
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

Status Journal::begin_run(bool resume) {
  std::lock_guard lock(mutex_);
  if (!resume) {
    Status s = exec("DELETE FROM done_dirs");
    if (!s.ok()) return s;
  }
  Status s = exec("INSERT INTO runs (started) VALUES (" + std::to_string(now_unix()) + ")");
  if (!s.ok()) return s;
  run_id_ = sqlite3_last_insert_rowid(db_);
  return {};
}

Status Journal::end_run(bool completed) {
  std::lock_guard lock(mutex_);
  return exec("UPDATE runs SET finished = " + std::to_string(now_unix()) +
              ", completed = " + (completed ? "1" : "0") + " WHERE id = " +
              std::to_string(run_id_));
}

void Journal::record_failure(const Failure& failure) {
  std::lock_guard lock(mutex_);
  bind_text(insert_failure_, 1, failure.path);
  sqlite3_bind_int(insert_failure_, 2, static_cast<int>(failure.type));
  sqlite3_bind_int(insert_failure_, 3, static_cast<int>(failure.error.kind));
  bind_text(insert_failure_, 4, failure.error.describe());
  sqlite3_bind_int64(insert_failure_, 5, run_id_);
  sqlite3_bind_int64(insert_failure_, 6, now_unix());
  if (step_done(insert_failure_) != SQLITE_OK)
    log::error("journal: cannot record failure of ", failure.path, ": ", sqlite3_errmsg(db_));
}

void Journal::clear_failure(const RelPath& path) {
  std::lock_guard lock(mutex_);
  bind_text(delete_failure_, 1, path);
  if (step_done(delete_failure_) != SQLITE_OK)
    log::error("journal: cannot clear failure of ", path, ": ", sqlite3_errmsg(db_));
}

std::vector<Failure> Journal::failures() {
  std::lock_guard lock(mutex_);
  std::vector<Failure> result;
  while (sqlite3_step(select_failures_) == SQLITE_ROW) {
    Failure f;
    f.path = reinterpret_cast<const char*>(sqlite3_column_text(select_failures_, 0));
    f.type = static_cast<EntryType>(sqlite3_column_int(select_failures_, 1));
    f.error.kind = static_cast<ErrorKind>(sqlite3_column_int(select_failures_, 2));
    f.error.message = reinterpret_cast<const char*>(sqlite3_column_text(select_failures_, 3));
    result.push_back(std::move(f));
  }
  sqlite3_reset(select_failures_);
  return result;
}

void Journal::record_done_dir(const RelPath& path) {
  std::lock_guard lock(mutex_);
  bind_text(insert_done_, 1, path);
  sqlite3_bind_int64(insert_done_, 2, run_id_);
  if (step_done(insert_done_) != SQLITE_OK)
    log::error("journal: cannot record directory ", path, ": ", sqlite3_errmsg(db_));
}

bool Journal::is_done_dir(const RelPath& path) {
  std::lock_guard lock(mutex_);
  bind_text(select_done_, 1, path);
  bool done = sqlite3_step(select_done_) == SQLITE_ROW;
  sqlite3_reset(select_done_);
  sqlite3_clear_bindings(select_done_);
  return done;
}

}  // namespace eosmirror
