// SPDX-License-Identifier: GPL-3.0-or-later
#include "eosmirror/selftest.hh"

#include <algorithm>

#include "doctest/doctest.h"
#include "eosmirror/posix_endpoint.hh"
#include "fake_endpoint.hh"
#include "temp_dir.hh"

using namespace eosmirror;

namespace {

const SelftestCheck* find(const SelftestReport& r, std::string_view name) {
  auto it = std::find_if(r.checks.begin(), r.checks.end(),
                         [&](const SelftestCheck& c) { return c.name == name; });
  return it == r.checks.end() ? nullptr : &*it;
}

}  // namespace

TEST_CASE("selftest passes on a local directory and leaves nothing behind") {
  TempDir tmp;
  PosixEndpoint target(tmp.path());
  SelftestReport r = run_selftest(target, is_root());
  for (const SelftestCheck& c : r.checks) {
    INFO(c.name, ": ", c.message);
    CHECK(c.ok);
  }
  CHECK(r.ok());
  REQUIRE(find(r, "verify the stored checksum"));
  CHECK(find(r, "verify the stored checksum")->skipped);  // no readback asked for
  CHECK(find(r, "symlink target"));
  CHECK(target.list("").value().empty());
}

TEST_CASE("selftest reports what an endpoint cannot do and what fails") {
  FakeEndpoint plain;
  plain.caps.has_symlinks = false;
  plain.caps.can_set_mtime = false;
  plain.caps.checksum = ChecksumType::Adler32;
  SelftestReport r = run_selftest(plain, true);
  CHECK(r.ok());
  REQUIRE(find(r, "create a symlink"));
  CHECK(find(r, "create a symlink")->skipped);
  CHECK_FALSE(find(r, "verify the stored checksum")->skipped);
  CHECK(plain.paths().empty());

  FakeEndpoint broken;
  broken.fail("commit", "", Error{ErrorKind::IO, "disk full"}, 100);
  broken.hook = [&](std::string_view op, const RelPath& path) {
    if (op == "commit") broken.fail("commit", path, Error{ErrorKind::IO, "disk full"});
  };
  SelftestReport r2 = run_selftest(broken, true);
  CHECK_FALSE(r2.ok());
  REQUIRE(find(r2, "commit a file"));
  CHECK_FALSE(find(r2, "commit a file")->ok);
  CHECK(find(r2, "commit a file")->message.find("disk full") != std::string::npos);
  CHECK(broken.paths().empty());  // the directory is still cleaned up
}
