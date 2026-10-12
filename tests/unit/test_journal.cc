// SPDX-License-Identifier: GPL-3.0-or-later
#include "eosmirror/journal.hh"

#include <sqlite3.h>

#include <algorithm>
#include <filesystem>
#include <vector>

#include "doctest/doctest.h"
#include "temp_dir.hh"

using namespace eosmirror;

TEST_CASE("journal keeps failures and finalized directories across openings") {
  TempDir tmp;
  std::string file = tmp.sub("j.sqlite");
  {
    auto opened = Journal::open(file, "/src", "root://host//dst");
    REQUIRE(opened.ok());
    Journal& j = *opened.value();
    REQUIRE(j.begin_run(false).ok());
    CHECK(j.run_id() == 1);
    for (const Failure& f :
         {Failure{"a/b", EntryType::File, Error{ErrorKind::IO, "read failed", 5}},
          Failure{"c", EntryType::Directory, Error{ErrorKind::Permission, "denied"}},
          Failure{"a/b", EntryType::File, Error{ErrorKind::Timeout, "again"}}})
      REQUIRE(j.record_failure(f).ok());
    REQUIRE(j.record_done_dir("").ok());
    REQUIRE(j.record_done_dir("a").ok());
    REQUIRE(j.end_run(false).ok());  // interrupted
  }
  {
    auto opened = Journal::open(file, "/src", "root://host//dst");
    REQUIRE(opened.ok());
    Journal& j = *opened.value();
    auto failures = j.failures().value();
    REQUIRE(failures.size() == 2);
    CHECK(failures[0].path == "a/b");
    CHECK(failures[0].type == EntryType::File);
    CHECK(failures[0].error.kind == ErrorKind::Timeout);  // the later record replaced the first
    CHECK(failures[0].error.message == "again");
    CHECK(failures[1].path == "c");
    CHECK(failures[1].type == EntryType::Directory);
    CHECK(j.is_done_dir("").value());
    CHECK(j.is_done_dir("a").value());
    CHECK_FALSE(j.is_done_dir("c").value());

    REQUIRE(j.begin_run(true).ok());
    CHECK(j.run_id() == 2);
    CHECK(j.is_done_dir("a").value());  // resuming an interrupted run keeps them
    REQUIRE(j.clear_failure("a/b").ok());
    REQUIRE(j.clear_failure("never recorded").ok());
    CHECK(j.failures().value().size() == 1);
    REQUIRE(j.end_run(true).ok());

    REQUIRE(j.begin_run(true).ok());
    CHECK_FALSE(j.is_done_dir("a").value());  // nothing to resume after a completed run
    REQUIRE(j.record_done_dir("a").ok());
    REQUIRE(j.end_run(false).ok());

    REQUIRE(j.begin_run(false).ok());
    CHECK_FALSE(j.is_done_dir("a").value());  // a fresh run forgets them
    CHECK(j.failures().value().size() == 1);  // but not the failures
  }

  auto wrong = Journal::open(file, "/other", "root://host//dst");
  REQUIRE_FALSE(wrong.ok());
  CHECK(wrong.error().message.find("belongs to /src -> root://host//dst") != std::string::npos);
  auto other_shard = Journal::open(file, "/src", "root://host//dst", "1/2");
  REQUIRE_FALSE(other_shard.ok());

  auto unusable = Journal::open(tmp.sub("missing/dir/j.sqlite"), "/src", "/dst");
  CHECK_FALSE(unusable.ok());
}

namespace {

// Runs SQL on a database file as another process would.
void sql(const std::string& file, const std::string& statements) {
  sqlite3* db = nullptr;
  REQUIRE(sqlite3_open(file.c_str(), &db) == SQLITE_OK);
  CHECK(sqlite3_exec(db, statements.c_str(), nullptr, nullptr, nullptr) == SQLITE_OK);
  sqlite3_close(db);
}

std::vector<std::string> files_in(const std::string& dir) {
  std::vector<std::string> names;
  for (const auto& e : std::filesystem::directory_iterator(dir))
    names.push_back(e.path().filename().string());
  std::sort(names.begin(), names.end());
  return names;
}

}  // namespace

TEST_CASE("journal: errors of reads and writes are no successes") {
  TempDir tmp;
  std::string file = tmp.sub("j.sqlite");
  auto opened = Journal::open(file, "/src", "/dst");
  REQUIRE(opened.ok());
  Journal& j = *opened.value();
  REQUIRE(j.begin_run(false).ok());
  sql(file, "CREATE TRIGGER full BEFORE INSERT ON failures BEGIN SELECT RAISE(FAIL, 'full'); END;");
  CHECK_FALSE(j.record_failure({"a", EntryType::File, Error{ErrorKind::IO, "x"}}).ok());
  sql(file, "DROP TABLE failures; DROP TABLE done_dirs;");
  CHECK_FALSE(j.failures().ok());
  CHECK_FALSE(j.is_done_dir("a").ok());
  CHECK_FALSE(j.record_done_dir("a").ok());
}

TEST_CASE("journal: read-only opens and other databases change nothing") {
  TempDir tmp;
  std::string file = tmp.sub("j.sqlite");
  {
    auto opened = Journal::open(file, "/src", "/dst");
    REQUIRE(opened.ok());
    REQUIRE(opened.value()->begin_run(false).ok());
    REQUIRE(opened.value()->record_failure({"a", EntryType::File, Error{ErrorKind::IO, "x"}}).ok());
    REQUIRE(opened.value()->end_run(true).ok());
  }
  auto before = files_in(tmp.path());
  {
    auto ro = Journal::open_read_only(file, "/src", "/dst");
    REQUIRE(ro.ok());
    CHECK(ro.value()->failures().value().size() == 1);
    CHECK_FALSE(ro.value()->interrupted().value());
    CHECK_FALSE(ro.value()->record_failure({"b", EntryType::File, Error{}}).ok());
    CHECK_FALSE(Journal::open_read_only(file, "/src", "/elsewhere").ok());
    CHECK(Journal::open_read_only(file).ok());
  }
  CHECK(files_in(tmp.path()) == before);

  // Databases of others, and files that are none, are refused as they are.
  std::string other = tmp.sub("other.sqlite"), with_meta = tmp.sub("meta.sqlite");
  sql(other, "CREATE TABLE notes (x TEXT)");
  sql(with_meta, "CREATE TABLE meta (key TEXT PRIMARY KEY, value TEXT)");
  tmp.write_file("text", "not a database");
  for (const std::string& path : {other, with_meta, tmp.sub("text")}) {
    INFO("file ", path);
    auto size = std::filesystem::file_size(path);
    auto ro = Journal::open_read_only(path);
    REQUIRE_FALSE(ro.ok());
    CHECK(ro.error().message.find("not an eosmirror journal") != std::string::npos);
    CHECK_FALSE(Journal::open(path, "/src", "/dst").ok());
    CHECK(std::filesystem::file_size(path) == size);
  }
  CHECK(files_in(tmp.path()) ==
        std::vector<std::string>{"j.sqlite", "meta.sqlite", "other.sqlite", "text"});
}

TEST_CASE("journal: the options of the last run") {
  TempDir tmp;
  auto opened = Journal::open(tmp.sub("j.sqlite"), "/src", "/dst");
  REQUIRE(opened.ok());
  Journal& j = *opened.value();
  CHECK(j.differing_options({{"--delete", "yes"}}).value().empty());  // none recorded
  REQUIRE(j.begin_run(false, {{"--delete", "no"}, {"--no-owner", "yes"}}).ok());
  REQUIRE(j.end_run(false).ok());
  CHECK(j.interrupted().value());
  auto differing =
      j.differing_options({{"--delete", "yes"}, {"--no-owner", "yes"}, {"--new", "x"}});
  CHECK(differing.value() == std::vector<std::string>{"--delete"});
}
