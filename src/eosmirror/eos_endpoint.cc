// SPDX-License-Identifier: GPL-3.0-or-later
#include "eosmirror/eos_endpoint.hh"

#include <XrdCl/XrdClFile.hh>
#include <XrdCl/XrdClFileSystem.hh>
#include <XrdCl/XrdClURL.hh>
#include <XrdCl/XrdClXRootDResponses.hh>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>

#include "eosmirror/log.hh"

namespace eosmirror {

namespace {

constexpr std::string_view kApp = "&eos.app=eosmirror";

// ---- encoding helpers -----------------------------------------------------------

// Percent-encodes everything but unreserved characters, like curl does.
std::string curl_escape(std::string_view s) {
  std::string out;
  out.reserve(s.size() * 3);
  for (unsigned char c : s) {
    if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~' || c == '/') {
      out += static_cast<char>(c);
    } else {
      char buf[4];
      std::snprintf(buf, sizeof buf, "%%%02X", c);
      out += buf;
    }
  }
  return out;
}

std::string curl_unescape(std::string_view s) {
  std::string out;
  out.reserve(s.size());
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '%' && i + 2 < s.size() && std::isxdigit(static_cast<unsigned char>(s[i + 1])) &&
        std::isxdigit(static_cast<unsigned char>(s[i + 2]))) {
      out += static_cast<char>(std::strtol(std::string(s.substr(i + 1, 2)).c_str(), nullptr, 16));
      i += 2;
    } else {
      out += s[i];
    }
  }
  return out;
}

std::string base64(std::string_view data) {
  static const char* const kAlphabet =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  out.reserve((data.size() + 2) / 3 * 4);
  size_t i = 0;
  while (i + 3 <= data.size()) {
    uint32_t v = (static_cast<uint8_t>(data[i]) << 16) | (static_cast<uint8_t>(data[i + 1]) << 8) |
                 static_cast<uint8_t>(data[i + 2]);
    out += kAlphabet[(v >> 18) & 63];
    out += kAlphabet[(v >> 12) & 63];
    out += kAlphabet[(v >> 6) & 63];
    out += kAlphabet[v & 63];
    i += 3;
  }
  if (i + 1 == data.size()) {
    uint32_t v = static_cast<uint8_t>(data[i]) << 16;
    out += kAlphabet[(v >> 18) & 63];
    out += kAlphabet[(v >> 12) & 63];
    out += "==";
  } else if (i + 2 == data.size()) {
    uint32_t v = (static_cast<uint8_t>(data[i]) << 16) | (static_cast<uint8_t>(data[i + 1]) << 8);
    out += kAlphabet[(v >> 18) & 63];
    out += kAlphabet[(v >> 12) & 63];
    out += kAlphabet[(v >> 6) & 63];
    out += '=';
  }
  return out;
}

// Minimal protobuf wire encoding, enough for the console's find request.
void put_varint(std::string& out, uint64_t v) {
  while (v >= 0x80) {
    out += static_cast<char>((v & 0x7f) | 0x80);
    v >>= 7;
  }
  out += static_cast<char>(v);
}
void put_bool(std::string& out, int field, bool v) {
  put_varint(out, static_cast<uint64_t>(field) << 3);
  put_varint(out, v ? 1 : 0);
}
void put_uint(std::string& out, int field, uint64_t v) {
  put_varint(out, static_cast<uint64_t>(field) << 3);
  put_varint(out, v);
}
void put_bytes(std::string& out, int field, std::string_view s) {
  put_varint(out, (static_cast<uint64_t>(field) << 3) | 2);
  put_varint(out, s.size());
  out.append(s);
}

// A RequestProto with a FindProto (field 5) listing one directory level with
// the given format (eos-protobuf-spec/Find.proto for the field numbers).
std::string find_request(const std::string& path, const std::string& format) {
  std::string find;
  put_bool(find, 1, true);    // Files
  put_bool(find, 2, true);    // Directories
  put_uint(find, 35, 1);      // Maxdepth (a oneof, so always sent)
  put_bytes(find, 43, path);  // Path
  put_bytes(find, 51, format);  // Format
  put_bool(find, 56, true);   // SkipVersionDirs
  std::string request;
  put_bytes(request, 5, find);
  return request;
}

// ---- parsing helpers ----------------------------------------------------------------

Timespec parse_timespec(std::string_view text) {
  Timespec t;
  auto dot = text.find('.');
  t.sec = std::strtoll(std::string(text.substr(0, dot)).c_str(), nullptr, 10);
  if (dot != std::string_view::npos) {
    std::string frac(text.substr(dot + 1));
    while (frac.size() < 9) frac += '0';
    t.nsec = static_cast<int32_t>(std::strtol(frac.substr(0, 9).c_str(), nullptr, 10));
  }
  return t;
}

// One "key=value key=value" line of find --format into an entry. Values are
// quoted when they may contain spaces (path, target).
bool parse_find_line(const std::string& line, Entry& e, std::string& abs_path) {
  size_t pos = 0;
  bool have_type = false;
  while (pos < line.size()) {
    while (pos < line.size() && line[pos] == ' ') ++pos;
    auto eq = line.find('=', pos);
    if (eq == std::string::npos) break;
    std::string key = line.substr(pos, eq - pos);
    std::string value;
    pos = eq + 1;
    if (pos < line.size() && line[pos] == '"') {
      auto end = line.find('"', pos + 1);
      if (end == std::string::npos) end = line.size();
      value = line.substr(pos + 1, end - pos - 1);
      pos = end + 1;
    } else {
      auto end = line.find(' ', pos);
      if (end == std::string::npos) end = line.size();
      value = line.substr(pos, end - pos);
      pos = end;
    }
    if (key == "path") {
      abs_path = value;
    } else if (key == "type") {
      have_type = true;
      if (value == "directory")
        e.type = EntryType::Directory;
      else if (value == "symlink")
        e.type = EntryType::Symlink;
      else
        e.type = EntryType::File;
    } else if (key == "size") {
      e.size = std::strtoull(value.c_str(), nullptr, 10);
    } else if (key == "uid") {
      e.uid = static_cast<uint32_t>(std::strtoul(value.c_str(), nullptr, 10));
    } else if (key == "gid") {
      e.gid = static_cast<uint32_t>(std::strtoul(value.c_str(), nullptr, 10));
    } else if (key == "mode" || key == "flags") {
      e.mode = static_cast<ModeBits>(std::strtoul(value.c_str(), nullptr, 8)) & 07777;
    } else if (key == "mtime") {
      e.mtime = parse_timespec(value);
    } else if (key == "target") {
      e.link_target = value;
    }
  }
  if (e.type == EntryType::Symlink) {
    e.mode = 0777;
    e.size = 0;
  }
  if (e.type == EntryType::Directory) e.size = 0;
  return have_type && !abs_path.empty();
}

std::string name_of(const std::string& path) {
  auto slash = path.rfind('/');
  return slash == std::string::npos ? path : path.substr(slash + 1);
}

std::string parent_of(const std::string& path) {
  auto slash = path.rfind('/');
  return slash == std::string::npos ? "" : path.substr(0, slash);
}

std::string format_timespec(const Timespec& t) {
  char buf[40];
  std::snprintf(buf, sizeof buf, "%lld.%09d", static_cast<long long>(t.sec), t.nsec);
  return buf;
}

// Reads through XRootD, but stats through the MGM: an open file's stat
// comes from the storage node, whose replica has only seconds of mtime.
class EosReader : public FileReader {
 public:
  EosReader(EosEndpoint& ep, std::unique_ptr<FileReader> inner, RelPath path)
      : ep_(ep), inner_(std::move(inner)), path_(std::move(path)) {}
  Result<size_t> read(uint64_t offset, std::span<std::byte> buf) override {
    return inner_->read(offset, buf);
  }
  Result<Entry> stat() override { return ep_.stat(path_); }

 private:
  EosEndpoint& ep_;
  std::unique_ptr<FileReader> inner_;
  RelPath path_;
};

// Writes through EOS's atomic upload: the file is created under its final
// name with eos.atomic=1 and eos.mtime=..., EOS stores it under a hidden
// name and renames it at close. Owner and mode are set afterwards.
class EosWriter : public FileWriter {
 public:
  EosWriter(EosEndpoint& ep, std::unique_ptr<FileWriter> inner, std::string abs, ChecksumType stored)
      : ep_(ep), inner_(std::move(inner)), abs_(std::move(abs)), stored_(stored) {}

  Status write(uint64_t offset, std::span<const std::byte> data) override {
    return inner_->write(offset, data);
  }

  Result<CommitInfo> commit(const CommitSpec& spec) override {
    bool verifiable = spec.checksum.type != ChecksumType::None && stored_ == spec.checksum.type;
    if (spec.require_verification && !verifiable) {
      abort();
      return Error{ErrorKind::Unsupported,
                   "the directory of " + abs_ + " computes no " +
                       std::string(to_string(spec.checksum.type)) + " checksum to verify against"};
    }
    CommitSpec inner_spec = spec;
    inner_spec.checksum = Checksum{};  // the directory decides the type, compared below
    auto committed = inner_->commit(inner_spec);
    if (!committed.ok()) return committed.error();
    if (verifiable) {
      auto stored = ep_.query_checksum(abs_);
      if (!stored.ok()) return stored.error();
      if (!(stored.value() == spec.checksum))
        return Error{ErrorKind::Checksum, abs_ + " has checksum " + stored.value().hex +
                                              " instead of " + spec.checksum.hex};
    }
    MetaFields fields = spec.fields & (MetaFields::Owner | MetaFields::Mode);
    Status md = ep_.set_metadata_abs(abs_, spec.metadata, fields, false);
    if (!md.ok()) return md.error();
    return CommitInfo{verifiable};
  }

  void abort() override { inner_->abort(); }

 private:
  EosEndpoint& ep_;
  std::unique_ptr<FileWriter> inner_;
  std::string abs_;
  ChecksumType stored_;
};

// The XRootD writer that EosWriter wraps: writes to the final URL with the
// atomic upload parameters, and does nothing at commit beyond closing.
class EosUploadWriter : public FileWriter {
 public:
  EosUploadWriter(std::unique_ptr<XrdCl::File> file, std::string url, int window)
      : file_(std::move(file)), url_(std::move(url)), window_(window) {}

  ~EosUploadWriter() override { abort(); }

  Status write(uint64_t offset, std::span<const std::byte> data) override {
    if (offset != written_) return Error{ErrorKind::Other, "non-sequential write to " + url_};
    XrdCl::XRootDStatus st =
        file_->Write(offset, static_cast<uint32_t>(data.size()), data.data());
    if (!st.IsOK()) return XrdEndpoint::xrd_error(st, "write " + url_);
    written_ += data.size();
    return {};
  }

  Result<CommitInfo> commit(const CommitSpec& spec) override {
    if (!file_) return Error{ErrorKind::Other, "commit without an open file"};
    if (written_ != spec.size) {
      abort();
      return Error{ErrorKind::Changed, "wrote " + std::to_string(written_) + " bytes to " + url_ +
                                           ", expected " + std::to_string(spec.size)};
    }
    XrdCl::XRootDStatus st = file_->Close();
    file_.reset();
    if (!st.IsOK()) return XrdEndpoint::xrd_error(st, "close " + url_);
    return CommitInfo{};
  }

  // Closing an atomic upload before its end discards it on the server.
  void abort() override {
    if (!file_) return;
    XrdCl::XRootDStatus st = file_->Close();
    (void)st;
    file_.reset();
  }

 private:
  std::unique_ptr<XrdCl::File> file_;
  std::string url_;
  int window_;
  uint64_t written_ = 0;
};

}  // namespace

// ---- EosEndpoint ------------------------------------------------------------------------

EosEndpoint::EosEndpoint(std::string url, std::string server, std::string root, XrdOptions options)
    : XrdEndpoint(std::move(url), std::move(server), std::move(root), options) {
  caps_.mtime_resolution = 1;
  caps_.can_set_mtime = true;
  caps_.has_symlinks = true;
  caps_.symlink_owner = false;
  caps_.can_set_mode = true;
  caps_.checksum = ChecksumType::Adler32;
}

Result<std::unique_ptr<EosEndpoint>> EosEndpoint::create(const std::string& url,
                                                         XrdOptions options) {
  XrdCl::URL parsed(url);
  if (!parsed.IsValid() || parsed.GetHostName().empty())
    return Error{ErrorKind::Other, "invalid EOS URL: " + url};
  std::string server = "root://" + parsed.GetHostId();
  std::string root = parsed.GetPath();
  while (root.size() > 1 && root.back() == '/') root.pop_back();
  if (root.empty() || root[0] != '/')
    return Error{ErrorKind::Other, "EOS URL needs an absolute path: " + url};
  std::unique_ptr<EosEndpoint> ep(new EosEndpoint(url, server, root, options));
  Status probed = ep->probe_identity();
  if (!probed.ok()) return probed.error();
  return ep;
}

bool EosEndpoint::is_eos(const std::string& url) {
  XrdCl::URL parsed(url);
  if (!parsed.IsValid() || parsed.GetHostName().empty()) return false;
  std::unique_ptr<EosEndpoint> ep(new EosEndpoint(url, "root://" + parsed.GetHostId(), "/", {}));
  auto result = ep->proc("mgm.cmd=whoami");
  return result.ok() && result.value().out.find("Virtual Identity") != std::string::npos;
}

// Finds out who EOS takes us for: root and sudoers may set owners.
Status EosEndpoint::probe_identity() {
  auto result = proc("mgm.cmd=whoami");
  if (!result.ok()) return result.error();
  const std::string& out = result.value().out;
  if (out.find("Virtual Identity") == std::string::npos)
    return Error{ErrorKind::Other, url_ + " is not an EOS instance (whoami: " + out + ")"};
  bool root = out.find("uid=0 ") != std::string::npos;
  bool sudoer = out.find("sudo*") != std::string::npos;
  caps_.can_set_owner = root || sudoer;
  log::debug(url_, ": ", out);
  return {};
}

Result<EosEndpoint::ProcResult> EosEndpoint::proc(const std::string& query) {
  std::string url = server_ + "//proc/user/?" + query + std::string(kApp);
  XrdCl::File file;
  XrdCl::XRootDStatus st = file.Open(url, XrdCl::OpenFlags::Read);
  if (!st.IsOK()) return xrd_error(st, "command on " + server_);
  std::string response;
  std::vector<char> buf(1 << 20);
  uint64_t offset = 0;
  for (;;) {
    uint32_t got = 0;
    st = file.Read(offset, static_cast<uint32_t>(buf.size()), buf.data(), got);
    if (!st.IsOK()) {
      XrdCl::XRootDStatus closed = file.Close();
      (void)closed;
      return xrd_error(st, "command response from " + server_);
    }
    if (got == 0) break;
    response.append(buf.data(), got);
    offset += got;
  }
  st = file.Close();
  (void)st;

  ProcResult result;
  auto out_pos = response.find("mgm.proc.stdout=");
  auto err_pos = response.find("&mgm.proc.stderr=");
  auto retc_pos = response.find("&mgm.proc.retc=");
  if (out_pos == std::string::npos || retc_pos == std::string::npos) {
    result.out = response;
    return result;
  }
  size_t out_end = err_pos != std::string::npos ? err_pos : retc_pos;
  result.out = response.substr(out_pos + 16, out_end - out_pos - 16);
  if (err_pos != std::string::npos)
    result.err = response.substr(err_pos + 17, retc_pos - err_pos - 17);
  result.retc = std::atoi(response.c_str() + retc_pos + 15);
  return result;
}

bool EosEndpoint::is_temporary(std::string_view name) const {
  return XrdEndpoint::is_temporary(name) || name.rfind(".sys.a#.", 0) == 0;
}

Result<Entry> EosEndpoint::stat(const RelPath& path) {
  std::string abs = absolute(path);
  XrdCl::Buffer arg;
  arg.FromString(curl_escape(abs) + "?mgm.pcmd=stat&eos.encodepath=1" + std::string(kApp));
  XrdCl::Buffer* response = nullptr;
  XrdCl::XRootDStatus st = fs_->Query(XrdCl::QueryCode::OpaqueFile, arg, response);
  if (!st.IsOK()) return xrd_error(st, "stat " + url_of(abs));
  std::unique_ptr<XrdCl::Buffer> owned(response);
  std::string text = response ? response->ToString() : "";

  // "stat: dev ino mode nlink uid gid rdev size blksize blocks atime mtime ctime
  //  atime_ns mtime_ns ctime_ns", or "stat: retc=N".
  if (text.rfind("stat: retc=", 0) == 0)
    return errno_error(std::atoi(text.c_str() + 11), "stat " + url_of(abs));
  unsigned long long v[16];
  if (std::sscanf(text.c_str(), "stat: %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu",
                  &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7], &v[8], &v[9], &v[10],
                  &v[11], &v[12], &v[13], &v[14], &v[15]) != 16)
    return Error{ErrorKind::Other, "unexpected stat response for " + url_of(abs) + ": " + text};
  Entry e;
  e.name = path.empty() ? "" : name_of(path);
  auto mode = static_cast<unsigned>(v[2]);
  switch (mode & 0170000) {
    case 0040000: e.type = EntryType::Directory; break;
    case 0120000: e.type = EntryType::Symlink; break;
    case 0100000: e.type = EntryType::File; break;
    default: e.type = EntryType::Other; break;
  }
  e.mode = e.type == EntryType::Symlink ? 0777 : (mode & 07777);
  e.uid = static_cast<uint32_t>(v[4]);
  e.gid = static_cast<uint32_t>(v[5]);
  if (e.type == EntryType::File) e.size = v[7];
  e.mtime = {static_cast<int64_t>(v[11]), static_cast<int32_t>(v[14])};
  if (e.type == EntryType::Symlink) {
    XrdCl::Buffer link_arg;
    link_arg.FromString(curl_escape(abs) + "?mgm.pcmd=readlink&eos.encodepath=1" +
                        std::string(kApp));
    XrdCl::Buffer* link_response = nullptr;
    st = fs_->Query(XrdCl::QueryCode::OpaqueFile, link_arg, link_response);
    if (!st.IsOK()) return xrd_error(st, "readlink " + url_of(abs));
    std::unique_ptr<XrdCl::Buffer> owned_link(link_response);
    std::string link = link_response ? link_response->ToString() : "";
    // "readlink: retc=0 /#curl#<escaped target>"
    auto marker = link.find("/#curl#");
    if (link.rfind("readlink: retc=0", 0) != 0 || marker == std::string::npos)
      return Error{ErrorKind::Other, "unexpected readlink response for " + url_of(abs) + ": " + link};
    std::string target = link.substr(marker + 7);
    while (!target.empty() && (target.back() == '\n' || target.back() == ' ')) target.pop_back();
    e.link_target = curl_unescape(target);
  }
  return e;
}

Result<std::vector<Entry>> EosEndpoint::list(const RelPath& dir) {
  std::string abs = absolute(dir);
  std::string request = find_request(abs, "type,size,uid,gid,mode,flags,mtime,link");
  auto result = proc("mgm.cmd.proto=" + base64(request));
  if (!result.ok()) return result.error();
  if (result.value().retc != 0)
    return errno_error(result.value().retc, "list " + url_of(abs) + ": " + result.value().err);

  std::vector<Entry> entries;
  std::istringstream lines(result.value().out);
  std::string line;
  while (std::getline(lines, line)) {
    if (line.empty()) continue;
    Entry e;
    std::string path;
    if (!parse_find_line(line, e, path)) {
      log::warn("unparsable listing line from ", url_of(abs), ": ", line);
      continue;
    }
    while (path.size() > 1 && path.back() == '/') path.pop_back();
    if (path == abs) continue;  // the directory itself
    e.name = name_of(path);
    if (e.name.empty() || e.name == "." || e.name == "..") continue;
    entries.push_back(std::move(e));
  }
  return entries;
}

Status EosEndpoint::symlink(const RelPath& path, const std::string& target) {
  std::string abs = absolute(path);
  if (target.find('&') != std::string::npos)
    return Error{ErrorKind::Unsupported, "EOS cannot store a symlink target containing '&': " + abs};
  // EOS refuses to replace a symlink, so an existing one is removed first.
  auto existing = stat(path);
  if (existing.ok() && existing.value().type == EntryType::Symlink) {
    Status removed = remove_abs(abs);
    if (!removed.ok() && removed.error().kind != ErrorKind::NotFound) return removed;
  }
  auto result = proc("mgm.cmd=file&mgm.subcmd=symlink&mgm.path=" + curl_escape(abs) +
                     "&eos.encodepath=1&mgm.file.source=" + abs + "&mgm.file.target=" + target);
  if (!result.ok()) return result.error();
  if (result.value().retc != 0)
    return errno_error(result.value().retc, "symlink " + url_of(abs) + ": " + result.value().err);
  return {};
}

Status EosEndpoint::set_metadata(const RelPath& path, const Entry& md, MetaFields fields) {
  return set_metadata_abs(absolute(path), md, fields, md.type == EntryType::Symlink);
}

Status EosEndpoint::set_metadata_abs(const std::string& abs, const Entry& md, MetaFields fields,
                                     bool is_symlink) {
  // Owners and modes of symlinks are not EOS's to set (chown follows the link).
  if (has(fields, MetaFields::Owner) && !is_symlink) {
    auto result = proc("mgm.cmd=chown&mgm.path=" + curl_escape(abs) + "&eos.encodepath=1" +
                       "&mgm.chown.owner=" + std::to_string(md.uid) + ":" + std::to_string(md.gid));
    if (!result.ok()) return result.error();
    if (result.value().retc != 0)
      return errno_error(result.value().retc, "chown " + url_of(abs) + ": " + result.value().err);
  }
  if (has(fields, MetaFields::Mode) && !is_symlink) {
    char mode[8];
    std::snprintf(mode, sizeof mode, "%o", md.mode & 07777);
    auto result = proc("mgm.cmd=chmod&mgm.path=" + curl_escape(abs) + "&eos.encodepath=1" +
                       "&mgm.chmod.mode=" + mode);
    if (!result.ok()) return result.error();
    if (result.value().retc != 0)
      return errno_error(result.value().retc, "chmod " + url_of(abs) + ": " + result.value().err);
  }
  if (has(fields, MetaFields::Mtime)) {
    char nsec[16];
    std::snprintf(nsec, sizeof nsec, "%09d", md.mtime.nsec);
    XrdCl::Buffer arg;
    arg.FromString(curl_escape(abs) + "?mgm.pcmd=utimes&tv1_sec=0&tv1_nsec=0&tv2_sec=" +
                   std::to_string(md.mtime.sec) + "&tv2_nsec=" + nsec + "&eos.encodepath=1" +
                   std::string(kApp));
    XrdCl::Buffer* response = nullptr;
    XrdCl::XRootDStatus st = fs_->Query(XrdCl::QueryCode::OpaqueFile, arg, response);
    if (!st.IsOK()) return xrd_error(st, "utimes " + url_of(abs));
    std::unique_ptr<XrdCl::Buffer> owned(response);
    std::string text = response ? response->ToString() : "";
    if (text.rfind("utimes: retc=0", 0) != 0) {
      int retc = text.rfind("utimes: retc=", 0) == 0 ? std::atoi(text.c_str() + 13) : EIO;
      return errno_error(retc ? retc : EIO, "utimes " + url_of(abs) + ": " + text);
    }
  }
  return {};
}

Result<ChecksumType> EosEndpoint::directory_checksum(const std::string& abs_dir) {
  {
    std::lock_guard lock(cache_mutex_);
    auto it = directory_checksums_.find(abs_dir);
    if (it != directory_checksums_.end()) return it->second;
  }
  auto result = proc("mgm.cmd=attr&mgm.subcmd=get&mgm.attr.key=sys.forced.checksum&mgm.path=" +
                     curl_escape(abs_dir) + "&eos.encodepath=1");
  if (!result.ok()) return result.error();
  ChecksumType type = ChecksumType::Adler32;  // EOS's default
  if (result.value().retc == 0) {
    // sys.forced.checksum="adler"
    std::string out = result.value().out;
    auto q1 = out.find('"');
    auto q2 = q1 == std::string::npos ? q1 : out.find('"', q1 + 1);
    if (q1 != std::string::npos && q2 != std::string::npos) {
      std::string name = out.substr(q1 + 1, q2 - q1 - 1);
      auto parsed = parse_checksum_type(name);
      type = parsed ? *parsed : ChecksumType::None;
    }
  }
  std::lock_guard lock(cache_mutex_);
  directory_checksums_[abs_dir] = type;
  return type;
}

Result<std::unique_ptr<FileReader>> EosEndpoint::open_read(const RelPath& path) {
  auto inner = XrdEndpoint::open_read(path);
  if (!inner.ok()) return inner.error();
  return std::unique_ptr<FileReader>(new EosReader(*this, std::move(inner).value(), path));
}

Result<std::unique_ptr<FileWriter>> EosEndpoint::open_write(const RelPath& path,
                                                            const CommitSpec& spec) {
  std::string abs = absolute(path);
  auto stored = directory_checksum(parent_of(abs));
  if (!stored.ok()) return stored.error();
  std::string url = url_of(abs) + "?eos.atomic=1" + std::string(kApp);
  if (has(spec.fields, MetaFields::Mtime)) url += "&eos.mtime=" + format_timespec(spec.metadata.mtime);
  auto file = std::make_unique<XrdCl::File>();
  ModeBits mode = has(spec.fields, MetaFields::Mode) ? spec.metadata.mode : 0644;
  XrdCl::XRootDStatus st = file->Open(url, XrdCl::OpenFlags::Delete | XrdCl::OpenFlags::Write,
                                      static_cast<XrdCl::Access::Mode>(mode & 0777));
  if (!st.IsOK()) return xrd_error(st, "create " + url_of(abs));
  std::unique_ptr<FileWriter> upload(
      new EosUploadWriter(std::move(file), url_of(abs), options_.write_window));
  return std::unique_ptr<FileWriter>(new EosWriter(*this, std::move(upload), abs, stored.value()));
}

}  // namespace eosmirror
