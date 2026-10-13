// SPDX-License-Identifier: GPL-3.0-or-later
//
// The requests and replies of the XRootD and EOS endpoints, without a server.
#include <XProtocol/XProtocol.hh>
#include <XrdCl/XrdClXRootDResponses.hh>

#include <cerrno>
#include <map>

#include "doctest/doctest.h"
#include "eosmirror/eos_endpoint.hh"

using namespace eosmirror;

namespace {

Entry listed(std::string_view line, std::string_view dir = "/eos/d") {
  INFO("line ", line);
  auto parsed = parse_find_line(line, dir);
  REQUIRE(parsed.ok());
  REQUIRE_FALSE(parsed.value().entry.name.empty());
  return parsed.value().entry;
}

bool refused(std::string_view line, std::string_view dir = "/eos/d") {
  return !parse_find_line(line, dir).ok();
}

}  // namespace

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
  CHECK(request == "/#curl#/eos/d/x%3Feos.ruid%3D0%26eos.rgid%3D0?eos.encodepath=1&eos.app=eosmirror");
  CHECK(request.find('?') == request.rfind('?'));
  CHECK(request.substr(0, request.find('?')).find('&') == std::string::npos);
}

TEST_CASE("endpoint URLs keep their protocol and opaque parameters") {
  auto parts = parse_endpoint_url("roots://user@host:1095//eos/dir/?authz=a%3Db&xrd.wantprot=krb5");
  REQUIRE(parts.ok());
  CHECK(parts.value().server == "roots://user@host:1095");
  CHECK(parts.value().host == "host:1095");
  CHECK(parts.value().path == "/eos/dir");
  CHECK(parts.value().cgi == "authz=a%3Db&xrd.wantprot=krb5");
  CHECK(display_url("roots://user@host:1095//eos/dir/?authz=a%3Db") == "roots://user@host:1095//eos/dir/");

  parts = parse_endpoint_url("root://Host.Example.org//data");
  REQUIRE(parts.ok());
  CHECK(parts.value().server == "root://Host.Example.org:1094");
  CHECK(parts.value().host == "host.example.org:1094");
  CHECK(parts.value().path == "/data");
  CHECK(parts.value().cgi.empty());
  CHECK_FALSE(parse_endpoint_url("root://host/relative").ok());
  // Servers resolve . and .. after symlinks, which comparing roots cannot.
  for (const char* url : {"root://host//eos/missing/../alias/p", "root://host//data/./x",
                          "root://host//data/..", "root://host//data/x/.?authz=a"})
    CHECK_FALSE(parse_endpoint_url(url).ok());
  CHECK(parse_endpoint_url("root://host//data/.hidden/..x/...").ok());

  // The parameters go after those of the request itself.
  std::string cgi = "authz=token&xrd.wantprot=krb5";
  CHECK(with_cgi("/data/f", cgi) == "/data/f?authz=token&xrd.wantprot=krb5");
  CHECK(with_cgi("/data/f", "") == "/data/f");
  CHECK(with_cgi(eos_request_path("/eos/a b"), cgi) ==
        "/#curl#/eos/a%20b?eos.encodepath=1&eos.app=eosmirror&authz=token&xrd.wantprot=krb5");
  CHECK(with_cgi("root://mgm//proc/user/?mgm.cmd=whoami", cgi) ==
        "root://mgm//proc/user/?mgm.cmd=whoami&authz=token&xrd.wantprot=krb5");
}

TEST_CASE("files are replaced by moves where the server refuses to replace them") {
  using Files = std::map<std::string, std::string>;
  Files files;
  bool lose_new = false;  // the new file cannot be moved in
  auto move = [&](const std::string& from, const std::string& to) -> Status {
    if (!files.count(from)) return Error{ErrorKind::NotFound, "no " + from};
    if (files.count(to)) return Error{ErrorKind::Exists, to + " exists"};
    if (lose_new && from == "/d/.f.tmp") return Error{ErrorKind::IO, "connection lost"};
    files[to] = files[from];
    files.erase(from);
    return {};
  };
  auto remove = [&](const std::string& path) -> Status {
    files.erase(path);
    return {};
  };

  files = {{"/d/.f.tmp", "new"}};
  REQUIRE(move_replacing("/d/.f.tmp", "/d/f", move, remove).ok());
  CHECK(files == Files{{"/d/f", "new"}});

  files = {{"/d/.f.tmp", "new"}, {"/d/f", "old"}};
  REQUIRE(move_replacing("/d/.f.tmp", "/d/f", move, remove).ok());
  CHECK(files == Files{{"/d/f", "new"}});

  // The old file goes back where the new one cannot follow.
  files = {{"/d/.f.tmp", "new"}, {"/d/f", "old"}};
  lose_new = true;
  auto failed = move_replacing("/d/.f.tmp", "/d/f", move, remove);
  REQUIRE_FALSE(failed.ok());
  CHECK(failed.error().kind == ErrorKind::IO);
  CHECK(files == Files{{"/d/.f.tmp", "new"}, {"/d/f", "old"}});
}

TEST_CASE("XRootD errors are classified for retries") {
  auto kind = [](uint16_t code, uint32_t errnum = 0) {
    return XrdEndpoint::xrd_error(XrdCl::XRootDStatus(XrdCl::stError, code, errnum, "x"), "op").kind;
  };
  auto server = [&](uint32_t xerr) { return kind(XrdCl::errErrorResponse, xerr); };
  for (uint32_t busy : {kXR_Overloaded, kXR_ServerError, kXR_noReplicas, kXR_inProgress, kXR_FSError}) {
    INFO("server error ", busy);
    CHECK(server(busy) == ErrorKind::IO);
  }
  CHECK(server(kXR_NotFound) == ErrorKind::NotFound);
  CHECK(server(kXR_NotAuthorized) == ErrorKind::Permission);
  CHECK(server(kXR_NoSpace) == ErrorKind::NoSpace);
  CHECK(server(kXR_overQuota) == ErrorKind::NoSpace);
  CHECK(server(kXR_ArgInvalid) == ErrorKind::Other);
  // A storage node's failure without an errno of its own, as protocol code
  // and as errno.
  CHECK(server(kXR_FSError) == ErrorKind::IO);
  CHECK(server(ENODEV) == ErrorKind::IO);
  CHECK(kind(XrdCl::errOSError, ENODEV) == ErrorKind::IO);
  for (uint16_t code : {XrdCl::errTlsError, XrdCl::errHandShakeFailed, XrdCl::errInvalidAddr,
                        XrdCl::errNoMoreFreeSIDs, XrdCl::errSocketTimeout, XrdCl::errConnectionError}) {
    INFO("client error ", code);
    CHECK(is_transient(kind(code)));
  }
  CHECK(kind(XrdCl::errAuthFailed) == ErrorKind::Permission);
  CHECK(kind(XrdCl::errLocalError, ENOSPC) == ErrorKind::NoSpace);
}

TEST_CASE("symlinks are created through the protobuf file command") {
  // RequestProto.file (29) { md (1) { path (1) }, symlink (12) { target_path (1), force (2) } }
  using namespace std::string_literals;
  CHECK(symlink_request("/e", "t") == "\xea\x01\x0d\x0a\x04\x0a\x02/e\x62\x05\x0a\x01t\x10\x01"s);
  std::string target = "a&b=c?d%e #f\"g'h";
  std::string request = symlink_request("/eos/d/l", target);
  CHECK(request.find(target) != std::string::npos);

  CHECK(symlink_safe("/eos/d/a&b=c?d%e #f", "../x&y=z?%25 \"q\""));
  CHECK(symlink_safe("/eos/d/l", "pid:5"));
  CHECK_FALSE(symlink_safe("/eos/d/a#AND#b", "t"));
  for (const char* id : {"fid:12", "fxid:ab", "cid:3", "cxid:4"}) CHECK_FALSE(symlink_safe("/eos/d/l", id));
  CHECK_FALSE(symlink_safe("/eos/d/l", "line\nbreak"));
}

TEST_CASE("subdirectories that find leaves out for lack of permission") {
  auto names = parse_find_denials(
      "error(13): no permissions to read directory /eos/d/secret/\n"
      "error(13): public access level restriction on directory /eos/d/deep dir/\n",
      "/eos/d");
  REQUIRE(names);
  CHECK(*names == std::vector<std::string>{"secret", "deep dir"});
  CHECK(parse_find_denials("error(13): no permissions to read directory /x/\n", "/") ==
        std::vector<std::string>{"x"});
  // The directory itself, entries elsewhere and anything else are not.
  CHECK_FALSE(parse_find_denials("error(13): no permissions to read directory /eos/d/\n", "/eos/d"));
  CHECK_FALSE(parse_find_denials("error(13): no permissions to read directory /eos/e/x/\n", "/eos/d"));
  CHECK_FALSE(parse_find_denials("error(13): no permissions to read directory /eos/d/x/y/\n", "/eos/d"));
  CHECK_FALSE(parse_find_denials("error: unable to run find in directory\n", "/eos/d"));
  CHECK_FALSE(parse_find_denials("", "/eos/d"));
}

TEST_CASE("getfmd replies describe a file by its id, with its path") {
  // As EOS 5.5.2 replies, '&' in names written as "#AND#".
  auto reply = parse_getfmd_reply(
      "getfmd: retc=0 name=probe a#AND#b.txt&id=227423&ctime=1791876727&ctime_ns=653271963"
      "&mtime=1791876727&mtime_ns=656643000&size=5&cid=18&uid=0&gid=0&lid=6553874&flags=416"
      "&link=&location=7,1,&checksum=062c0215&container=/eos/test/replica2/");
  REQUIRE(reply.ok());
  CHECK(reply.value().path == "/eos/test/replica2/probe a#AND#b.txt");
  const Entry& f = reply.value().file;
  CHECK(f.type == EntryType::File);
  CHECK(f.id == "227423");
  CHECK(f.size == 5);
  CHECK(f.mtime == Timespec{1791876727, 656643000});
  CHECK(f.checksum == Checksum{ChecksumType::Adler32, "062c0215"});

  // A layout without checksum (kNone), or another type: no checksum.
  for (const char* lid : {"6553873", "6553875"}) {
    auto other = parse_getfmd_reply(
        std::string("getfmd: retc=0 name=x&id=1&mtime=2&mtime_ns=3&size=4&lid=") + lid +
        "&checksum=00000000&container=/eos/");
    REQUIRE(other.ok());
    CHECK(other.value().file.checksum.type == ChecksumType::None);
  }
  auto gone = parse_getfmd_reply(std::string("getfmd: retc=2\0", 15));
  REQUIRE_FALSE(gone.ok());
  CHECK(gone.error().kind == ErrorKind::NotFound);
  for (const char* bad : {"", "getfmd: retc=x", "stat: retc=0",
                          "getfmd: retc=0 name=x&id=1&mtime=2&mtime_ns=3&lid=2&container=/eos/",
                          "getfmd: retc=0 name=x&id=-1&mtime=2&mtime_ns=3&size=4&lid=2&container=/",
                          "getfmd: retc=0 name=x&id=1&mtime=2&mtime_ns=3&size=4&lid=2&container=e",
                          "getfmd: retc=0 name=x&id=1&mtime=2&mtime_ns=3000000000&size=4&lid=2"
                          "&container=/"}) {
    INFO("reply ", bad);
    auto parsed = parse_getfmd_reply(bad);
    REQUIRE_FALSE(parsed.ok());
    CHECK(parsed.error().kind == ErrorKind::Other);
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
  CHECK(listed(R"(path="/eos/d/t" type=file  size=0 uid=0 gid=0 flags=0 mtime=17 ctime=18.000000042)")
            .ctime == Timespec{18, 42});
  // Before 1970, as struct timespec: -1.5 s.
  CHECK(listed(R"(path="/eos/d/t" type=file  size=0 uid=0 gid=0 flags=0 mtime=-2.500000000)")
            .mtime == Timespec{-2, 500000000});

  CHECK(f.checksum == Checksum{});

  // Checksums with their type; files that have none, or one never stored.
  Entry c = listed(R"(path="/eos/d/c" type=file  size=5 uid=1 gid=2 flags=644 mtime=1.0 )"
                   R"(checksum=0A1b2c3d checksumtype=adler)");
  CHECK(c.checksum == Checksum{ChecksumType::Adler32, "0a1b2c3d"});
  CHECK(c.size == 5);
  for (const char* xs :
       {"checksum= checksumtype=none", "checksum=00000000 checksumtype=adler",
        "checksum=0a1b2c3d checksumtype=crc32c", "checksum=0a1b checksumtype=adler",
        "checksum=0a1b2c3x checksumtype=adler"}) {
    INFO(xs);
    CHECK(listed(std::string(R"(path="/eos/d/n" type=file  size=5 uid=1 gid=2 flags=644 )") +
                 "mtime=1.0 " + xs)
              .checksum == Checksum{});
  }

  Entry d = listed(R"(path="/eos/d/sub/" type=directory  size=4096 uid=5 gid=6 mode=40755 )"
                   R"(mtime=1600000000.5)");
  CHECK(d.name == "sub");
  CHECK(d.type == EntryType::Directory);
  CHECK(d.size == 0);
  CHECK(d.mode == 0755);
  CHECK(d.mtime == Timespec{1600000000, 500000000});

  Entry l = listed(R"(path="/eos/d/l" type=symlink  size=0 uid=1 gid=2 flags=0 mtime=1.0 )"
                   R"(checksum= checksumtype=none target="../a "quoted" target")");
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
    CHECK(self.value().entry.name.empty());
    CHECK(self.value().entry.type == EntryType::Directory);
    CHECK_FALSE(self.value().non_atomic);
  }
}

TEST_CASE("find listing lines tell directories that force uploads to be non-atomic") {
  auto non_atomic = [](std::string_view attributes) {
    std::string line =
        std::string(R"(path="/eos/d/" type=directory  size=0 uid=0 gid=0 mode=40755 mtime=1.0 )") +
        std::string(attributes);
    INFO("line ", line);
    auto parsed = parse_find_line(line, "/eos/d");
    REQUIRE(parsed.ok());
    return parsed.value().non_atomic;
  };
  CHECK(non_atomic(R"(attr.user.forced.atomic="0")"));
  CHECK(non_atomic(R"(attr.sys.forced.atomic="0")"));
  CHECK_FALSE(non_atomic(R"(attr.user.forced.atomic="1")"));
  CHECK_FALSE(non_atomic(""));
  // The system attribute decides where it is set.
  CHECK_FALSE(non_atomic(R"(attr.sys.forced.atomic="1" attr.user.forced.atomic="0")"));
  CHECK(non_atomic(R"(attr.sys.forced.atomic="0" attr.user.forced.atomic="1")"));
  // As EOS reads the values: a number that starts them, else 0.
  CHECK(non_atomic(R"(attr.user.forced.atomic="")"));
  CHECK(non_atomic(R"(attr.user.forced.atomic="no")"));
  CHECK_FALSE(non_atomic(R"(attr.user.forced.atomic="2x")"));
  CHECK_FALSE(non_atomic(R"(attr.user.forced.atomic="-1")"));
  // Values that would make the rest of the line ambiguous are refused.
  for (const char* value : {R"("0 size=5")", R"("0)", R"(0)", R"("0"x")"}) {
    INFO("value ", value);
    CHECK(refused(std::string(R"(path="/eos/d/s/" type=directory  size=0 uid=0 gid=0 )") +
                  "mode=40755 mtime=1.0 attr.user.forced.atomic=" + value));
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
  // Fields that cannot be parsed are no zeros.
  for (const char* fields :
       {"size=12x uid=1 gid=1 flags=644 mtime=1.0", "size=1 uid=-1 gid=1 flags=644 mtime=1.0",
        "size=1 uid=1 gid= flags=644 mtime=1.0", "size=1 uid=1 gid=1 flags=648 mtime=1.0",
        "size=99999999999999999999 uid=1 gid=1 flags=644 mtime=1.0",
        "size=1 uid=1 gid=1 flags=644 mtime=1.2.3", "size=1 uid=1 gid=1 flags=644 mtime=x",
        "size=1 uid=1 gid=1 flags=644 mtime=1600000000.1234567890",
        "size=1 uid=1 gid=1 flags=644 mtime=1. ctime=1.0", "size=1 uid=1 gid=1 flags=644 mtime=+1",
        "size=1 uid=1 gid=1 flags=644 mtime=--5.0", "size=1 uid=1 gid=1 flags=644 mtime=1.-5",
        "size=-1 uid=1 gid=1 flags=644 mtime=1.0"}) {
    INFO(fields);
    CHECK(refused(std::string(R"(path="/eos/d/f" type=file  )") + fields));
  }
}
