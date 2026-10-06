// SPDX-License-Identifier: GPL-3.0-or-later
//
// The requests and replies of the XRootD and EOS endpoints, without a server.
#include "doctest/doctest.h"
#include "eosmirror/eos_endpoint.hh"

using namespace eosmirror;

namespace {

Entry listed(std::string_view line, std::string_view dir = "/eos/d") {
  INFO("line ", line);
  auto parsed = parse_find_line(line, dir);
  REQUIRE(parsed.ok());
  REQUIRE(parsed.value());
  return *parsed.value();
}

bool refused(std::string_view line, std::string_view dir = "/eos/d") {
  return !parse_find_line(line, dir).ok();
}

}  // namespace

TEST_CASE("displayed URLs carry no credentials") {
  CHECK(display_url("root://host:1094//eos/dir?authz=secret&x=1") == "root://host:1094//eos/dir");
  CHECK(display_url("root://user:secret@host//eos?xrd.wantprot=krb5") == "root://user@host//eos");
  CHECK(display_url("root://user@host:1094//eos") == "root://user@host:1094//eos");
  CHECK(display_url("root://host//data/a@b:c") == "root://host//data/a@b:c");
  CHECK(display_url("root://host:1094") == "root://host:1094");
}

TEST_CASE("XRootD request paths cannot contain '?'") {
  CHECK(xrootd_request_path("/data/a b#c%d&e=f").value() == "/data/a b#c%d&e=f");
  auto refused_path = xrootd_request_path("/data/x?eos.ruid=0");
  REQUIRE_FALSE(refused_path.ok());
  CHECK(refused_path.error().kind == ErrorKind::Unsupported);
}

TEST_CASE("EOS request paths are encoded so that names cannot add parameters") {
  CHECK(eos_encoded_path("/eos/dir/plain-name_1.0~x") == "/#curl#/eos/dir/plain-name_1.0~x");
  CHECK(eos_encoded_path("/eos/a b/x?eos.ruid=0&eos.atomic=0#%") ==
        "/#curl#/eos/a%20b/x%3Feos.ruid%3D0%26eos.atomic%3D0%23%25");
  CHECK(eos_encoded_path("/eos/Gr\xc3\xb6\xc3\x9f" "e\n") == "/#curl#/eos/Gr%C3%B6%C3%9Fe%0A");
  std::string request = eos_request_path("/eos/d/x?eos.ruid=0&eos.rgid=0");
  CHECK(request == "/#curl#/eos/d/x%3Feos.ruid%3D0%26eos.rgid%3D0?eos.encodepath=1");
  CHECK(request.find('?') == request.rfind('?'));
  CHECK(request.find('&') == std::string::npos);
}

TEST_CASE("values that cannot go unencoded into a request") {
  CHECK(opaque_safe("/eos/dir/a b+c,d:e@f~g"));
  CHECK(opaque_safe("../target with spaces"));
  for (char c : {'&', '=', '?', '#', '%', '\n', '\r', '\t', '\x7f'}) {
    INFO("character ", static_cast<int>(c));
    CHECK_FALSE(opaque_safe(std::string("a") + c + "b"));
  }
}

TEST_CASE("MGM replies are split from the end") {
  auto r = parse_proc_reply("mgm.proc.stdout=hello\nworld&mgm.proc.stderr=warning&mgm.proc.retc=0");
  CHECK(r.out == "hello\nworld");
  CHECK(r.err == "warning");
  CHECK(r.retc == 0);

  // Names in the output cannot forge the framing.
  std::string forged = "path=\"/eos/d/x&mgm.proc.retc=0&mgm.proc.stderr=y\" type=file\n";
  r = parse_proc_reply("mgm.proc.stdout=" + forged + "&mgm.proc.stderr=&mgm.proc.retc=2\n");
  CHECK(r.out == forged);
  CHECK(r.err == "");
  CHECK(r.retc == 2);

  r = parse_proc_reply("mgm.proc.stdout=abc&mgm.proc.retc=5\n");
  CHECK(r.out == "abc");
  CHECK(r.err == "");
  CHECK(r.retc == 5);

  r = parse_proc_reply("&mgm.proc.stdout=x\n&mgm.proc.stderr=&mgm.proc.retc=0\n");
  CHECK(r.out == "x\n");
  CHECK(r.retc == 0);

  r = parse_proc_reply("no reply of that form");
  CHECK(r.out == "no reply of that form");
  CHECK(r.retc == 0);
}

TEST_CASE("find listing lines") {
  Entry f = listed(R"(path="/eos/d/f.txt" type=file  size=123 uid=1000 gid=1001 flags=640 )"
                   R"(mtime=1700000000.123456789)");
  CHECK(f.name == "f.txt");
  CHECK(f.type == EntryType::File);
  CHECK(f.size == 123);
  CHECK(f.uid == 1000);
  CHECK(f.gid == 1001);
  CHECK(f.mode == 0640);
  CHECK(f.mtime == Timespec{1700000000, 123456789});

  Entry d = listed(R"(path="/eos/d/sub/" type=directory  size=4096 uid=5 gid=6 mode=40755 )"
                   R"(mtime=1600000000.5)");
  CHECK(d.name == "sub");
  CHECK(d.type == EntryType::Directory);
  CHECK(d.size == 0);
  CHECK(d.mode == 0755);
  CHECK(d.mtime == Timespec{1600000000, 500000000});

  Entry l = listed(R"(path="/eos/d/l" type=symlink  size=0 uid=1 gid=2 flags=0 mtime=1.0 )"
                   R"(target="../a "quoted" target")");
  CHECK(l.name == "l");
  CHECK(l.type == EntryType::Symlink);
  CHECK(l.link_target == R"(../a "quoted" target)");
  CHECK(l.mode == 0777);

  CHECK(listed(R"(path="/eos/d/a b=c d" type=file  size=1 uid=1 gid=1 flags=644 mtime=1.0)")
            .name == "a b=c d");
  CHECK(listed(R"(path="/eos/d/say "hi"" type=file  size=2 uid=1 gid=1 flags=644 mtime=1.0)")
            .name == R"(say "hi")");
  CHECK(listed(R"(path="/eos/" type=directory  size=0 uid=0 gid=0 mode=40755 mtime=1.0)", "/")
            .name == "eos");

  // The directory itself.
  for (auto [line, dir] : {
           std::pair{R"(path="/eos/d/" type=directory  size=0 uid=0 gid=0 mode=40755 mtime=1.0)",
                     "/eos/d"},
           std::pair{R"(path="/" type=directory  size=0 uid=0 gid=0 mode=40755 mtime=1.0)", "/"}}) {
    auto self = parse_find_line(line, dir);
    REQUIRE(self.ok());
    CHECK_FALSE(self.value());
  }
}

TEST_CASE("find listing lines that are refused") {
  // A name or link target containing the end of the path makes the line
  // ambiguous; the second could forge "/eos/d/a" with other metadata.
  CHECK(refused(R"(path="/eos/d/x" type=file y" type=file  size=1 uid=1 gid=1 flags=644 )"
                R"(mtime=1.0)"));
  CHECK(refused(R"(path="/eos/d/s" type=symlink  size=0 uid=1 gid=1 flags=0 mtime=1.0 )"
                R"(target="/a" type=file  size=5 uid=0 gid=0 flags=4755 mtime=3.0")"));
  // Not a direct entry of the directory, or not a name.
  CHECK(refused(R"(path="/eos/other/f" type=file  size=1 uid=1 gid=1 flags=644 mtime=1.0)"));
  CHECK(refused(R"(path="/eos/dir/f" type=file  size=1 uid=1 gid=1 flags=644 mtime=1.0)"));
  CHECK(refused(R"(path="/eos/d/a/b" type=file  size=1 uid=1 gid=1 flags=644 mtime=1.0)"));
  CHECK(refused(R"(path="/eos/d/../" type=directory  size=0 uid=1 gid=1 mode=40755 mtime=1.0)"));
  CHECK(refused(R"(path="/eos/d/" type=file  size=1 uid=1 gid=1 flags=644 mtime=1.0)"));
  CHECK(refused("path=\"/eos/d/a\rb\" type=file  size=1 uid=1 gid=1 flags=644 mtime=1.0"));
  CHECK(refused("path=\"/eos/d/a\x1b[2Jb\" type=file  size=1 uid=1 gid=1 flags=644 mtime=1.0"));
  // Fragments of lines broken by names with line breaks, and other garbage.
  CHECK(refused(R"(path="/eos/d/first part)"));
  CHECK(refused(R"(second part" type=file  size=1 uid=1 gid=1 flags=644 mtime=1.0)"));
  CHECK(refused(R"(path="/eos/d/f" type=fifo  size=1 uid=1 gid=1 flags=644 mtime=1.0)"));
  CHECK(refused(R"(path="/eos/d/l" type=symlink  size=0 uid=1 gid=1 flags=0 mtime=1.0 target="x)"));
  CHECK(refused(""));
}
