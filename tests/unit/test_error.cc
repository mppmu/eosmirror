// SPDX-License-Identifier: GPL-3.0-or-later
#include "eosmirror/error.hh"

#include <cerrno>

#include "doctest/doctest.h"
#include "eosmirror/types.hh"

using namespace eosmirror;

TEST_CASE("errno mapping and transience") {
  CHECK(errno_error(ENOENT, "stat x").kind == ErrorKind::NotFound);
  CHECK(errno_error(EEXIST, "mkdir x").kind == ErrorKind::Exists);
  CHECK(errno_error(EACCES, "open x").kind == ErrorKind::Permission);
  CHECK(errno_error(EIO, "read x").kind == ErrorKind::IO);
  CHECK(errno_error(ENODEV, "write x").kind == ErrorKind::IO);
  CHECK(errno_error(EXDEV, "rename x").kind == ErrorKind::Other);
  CHECK(errno_error(ENOSPC, "write x").kind == ErrorKind::NoSpace);
  CHECK(errno_error(EDQUOT, "write x").kind == ErrorKind::NoSpace);
  CHECK(to_string(ErrorKind::NoSpace) == "no space");

  CHECK(is_transient(ErrorKind::IO));
  CHECK(is_transient(ErrorKind::Timeout));
  CHECK(is_transient(ErrorKind::Changed));
  CHECK_FALSE(is_transient(ErrorKind::NotFound));
  CHECK_FALSE(is_transient(ErrorKind::Permission));
  CHECK_FALSE(is_transient(ErrorKind::Cancelled));
  CHECK_FALSE(is_transient(ErrorKind::NoSpace));

  Error e = errno_error(ENOENT, "stat /x");
  CHECK(e.describe() == "stat /x: No such file or directory");
  CHECK(Error{ErrorKind::Other, "plain"}.describe() == "plain");
}

TEST_CASE("failed uploads are retried unless space or permission is missing") {
  for (int err : {ENODEV, ENOENT, EINVAL, EBADF, EXDEV, EIO}) {
    INFO("errno ", err);
    Error e = upload_error(errno_error(err, "write x"));
    CHECK(is_transient(e.kind));
    CHECK(e.errnum == err);
    CHECK(e.message == "write x");
  }
  CHECK(upload_error(Error{ErrorKind::Other, "close x"}).kind == ErrorKind::IO);
  CHECK(upload_error(Error{ErrorKind::Checksum, "x"}).kind == ErrorKind::Checksum);
  CHECK(upload_error(errno_error(ENOSPC, "write x")).kind == ErrorKind::NoSpace);
  CHECK(upload_error(errno_error(EDQUOT, "write x")).kind == ErrorKind::NoSpace);
  CHECK(upload_error(errno_error(EACCES, "write x")).kind == ErrorKind::Permission);
}

TEST_CASE("Result carries a value or an error") {
  Result<int> good(42);
  REQUIRE(good.ok());
  CHECK(good.value() == 42);

  Result<int> bad(Error{ErrorKind::IO, "boom"});
  REQUIRE_FALSE(bad);
  CHECK(bad.error().message == "boom");

  Status fine;
  CHECK(fine.ok());
  Status failed(Error{ErrorKind::Timeout, "slow"});
  CHECK_FALSE(failed.ok());
  CHECK(to_string(failed.error().kind) == "timeout");
}

TEST_CASE("path join and timespec helpers") {
  CHECK(join("", "a") == "a");
  CHECK(join("a", "b") == "a/b");
  CHECK(join("a/b", "c") == "a/b/c");

  Timespec t{10, 123456789};
  CHECK(t.truncated(1000000000) == Timespec{10, 0});
  CHECK(t.truncated(1) == t);
  CHECK(t.truncated(1000) == Timespec{10, 123456000});
  CHECK(Timespec{1, 5} < Timespec{2, 0});
  CHECK(Timespec{2, 0} < Timespec{2, 1});

  MetaFields f = MetaFields::Owner | MetaFields::Mtime;
  CHECK(has(f, MetaFields::Owner));
  CHECK_FALSE(has(f, MetaFields::Mode));
  CHECK(has(~f, MetaFields::Mode));
}
