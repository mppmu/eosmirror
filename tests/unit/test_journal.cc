// SPDX-License-Identifier: GPL-3.0-or-later
#include "eosmirror/journal.hh"

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
    j.record_failure({"a/b", EntryType::File, Error{ErrorKind::IO, "read failed", 5}});
    j.record_failure({"c", EntryType::Directory, Error{ErrorKind::Permission, "denied"}});
    j.record_failure({"a/b", EntryType::File, Error{ErrorKind::Timeout, "again"}});
    j.record_done_dir("");
    j.record_done_dir("a");
    REQUIRE(j.end_run(true).ok());
  }
  {
    auto opened = Journal::open(file, "/src", "root://host//dst");
    REQUIRE(opened.ok());
    Journal& j = *opened.value();
    auto failures = j.failures();
    REQUIRE(failures.size() == 2);
    CHECK(failures[0].path == "a/b");
    CHECK(failures[0].type == EntryType::File);
    CHECK(failures[0].error.kind == ErrorKind::Timeout);  // the later record replaced the first
    CHECK(failures[0].error.message == "again");
    CHECK(failures[1].path == "c");
    CHECK(failures[1].type == EntryType::Directory);
    CHECK(j.is_done_dir(""));
    CHECK(j.is_done_dir("a"));
    CHECK_FALSE(j.is_done_dir("c"));

    REQUIRE(j.begin_run(true).ok());
    CHECK(j.run_id() == 2);
    CHECK(j.is_done_dir("a"));  // resuming keeps them
    j.clear_failure("a/b");
    j.clear_failure("never recorded");
    CHECK(j.failures().size() == 1);

    REQUIRE(j.begin_run(false).ok());
    CHECK_FALSE(j.is_done_dir("a"));  // a fresh run forgets them
    CHECK(j.failures().size() == 1);  // but not the failures
  }

  auto wrong = Journal::open(file, "/other", "root://host//dst");
  REQUIRE_FALSE(wrong.ok());
  CHECK(wrong.error().message.find("belongs to /src -> root://host//dst") != std::string::npos);

  auto unusable = Journal::open(tmp.sub("missing/dir/j.sqlite"), "/src", "/dst");
  CHECK_FALSE(unusable.ok());
}
