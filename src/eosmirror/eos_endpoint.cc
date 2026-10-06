// SPDX-License-Identifier: GPL-3.0-or-later
#include "eosmirror/eos_endpoint.hh"

#include <XrdCl/XrdClFile.hh>
#include <XrdCl/XrdClFileSystem.hh>
#include <XrdCl/XrdClXRootDResponses.hh>

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

#include "eosmirror/log.hh"

namespace eosmirror {

namespace {

constexpr std::string_view kApp = "&eos.app=eosmirror";

// ---- encoding helpers -----------------------------------------------------------

// Percent-encodes everything but unreserved characters and '/', like EOS does.
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

// Minimal protobuf wire encoding, enough for the console's find and symlink
// requests.
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

template <class T>
T parse_number(std::string_view text, int base = 10) {
  T value = 0;
  std::from_chars(text.data(), text.data() + text.size(), value, base);
  return value;
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
// name and renames it at close. The stored checksum is then compared where
// EOS computed one of the type that was computed here, and owner and mode
// are set.
class EosWriter : public FileWriter {
 public:
  EosWriter(EosEndpoint& ep, std::unique_ptr<FileWriter> inner, std::string abs, std::string url)
      : ep_(ep), inner_(std::move(inner)), abs_(std::move(abs)), url_(std::move(url)) {}

  Status write(uint64_t offset, std::span<const std::byte> data) override {
    return inner_->write(offset, data);
  }

  Result<CommitInfo> commit(const CommitSpec& spec) override {
    CommitSpec inner_spec = spec;
    inner_spec.checksum = Checksum{};  // compared below
    auto committed = inner_->commit(inner_spec);
    if (!committed.ok()) return committed.error();
    // The file is in place now, with the source's size and mtime: what fails
    // from here on has to take it away again.
    bool verified = false;
    if (spec.checksum.type != ChecksumType::None) {
      auto stored = ep_.query_checksum(abs_);
      if (!stored.ok()) return discard(stored.error());
      if (stored.value().type == spec.checksum.type) {
        if (!(stored.value() == spec.checksum))
          return discard(Error{ErrorKind::Checksum, url_ + " has checksum " + stored.value().hex +
                                                        " instead of " + spec.checksum.hex});
        verified = true;
      }
    }
    if (spec.require_verification && !verified)
      return discard(Error{ErrorKind::Unsupported,
                           "EOS stores no " + std::string(to_string(spec.checksum.type)) +
                               " checksum to verify " + url_ + " against"});
    MetaFields fields = spec.fields & (MetaFields::Owner | MetaFields::Mode);
    Status md = ep_.set_metadata_abs(abs_, spec.metadata, fields, false);
    if (!md.ok()) return md.error();
    return CommitInfo{verified};
  }

  void abort() override { inner_->abort(); }

 private:
  // Removes the committed file, or else sets its mtime to 0 so that the next
  // run copies it again.
  Error discard(Error e) {
    Status removed = ep_.remove_abs(abs_);
    if (removed.ok() || removed.error().kind == ErrorKind::NotFound) return e;
    Entry md;
    md.mtime = {0, 0};
    Status reset = ep_.set_metadata_abs(abs_, md, MetaFields::Mtime, false);
    if (!reset.ok())
      log::error("cannot remove ", url_, " after a failed verification, nor reset its mtime: ",
                 removed.error().describe(), "; ", reset.error().describe());
    return e;
  }

  EosEndpoint& ep_;
  std::unique_ptr<FileWriter> inner_;
  std::string abs_;
  std::string url_;
};

// The XRootD writer that EosWriter wraps: writes to the final URL with the
// atomic upload parameters, one synchronous write at a time, and does
// nothing at commit beyond closing. Erasure-coded layouts need the writes in
// order (see docs/design.md).
class EosUploadWriter : public FileWriter {
 public:
  EosUploadWriter(std::unique_ptr<XrdCl::File> file, std::string url)
      : file_(std::move(file)), url_(std::move(url)) {}

  ~EosUploadWriter() override { abort(); }

  Status write(uint64_t offset, std::span<const std::byte> data) override {
    if (offset != written_) return Error{ErrorKind::Other, "non-sequential write to " + url_};
    XrdCl::XRootDStatus st =
        file_->Write(offset, static_cast<uint32_t>(data.size()), data.data());
    if (!st.IsOK()) return stalled(XrdEndpoint::xrd_error(st, "write " + url_));
    last_ack_ = std::chrono::steady_clock::now();
    written_ += data.size();
    return {};
  }

  Error stalled(Error e) const {
    auto secs = std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::steady_clock::now() - last_ack_)
                    .count();
    e.message += " (last write acknowledged " + std::to_string(secs) + " s earlier)";
    return e;
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
    if (!st.IsOK()) return stalled(XrdEndpoint::xrd_error(st, "close " + url_));
    return CommitInfo{};
  }

  // A storage node commits an atomic upload at any regular close, partial or
  // not; told to delete it first, it discards the upload instead. A file that
  // XrdCl gave up on is not closed at all, and the node discards its upload
  // when the connection ends.
  void abort() override {
    if (!file_) return;
    if (file_->IsOpen()) {
      XrdCl::Buffer arg;
      arg.FromString("delete");
      XrdCl::Buffer* response = nullptr;
      XrdCl::XRootDStatus st = file_->Fcntl(arg, response);
      delete response;
      if (!st.IsOK()) log::warn("cannot discard the upload of ", url_, ": ", st.ToStr());
      st = file_->Close();  // fails after the delete
    }
    file_.reset();
  }

 private:
  std::unique_ptr<XrdCl::File> file_;
  std::string url_;
  uint64_t written_ = 0;
  std::chrono::steady_clock::time_point last_ack_ = std::chrono::steady_clock::now();
};

}  // namespace

// ---- requests and replies -----------------------------------------------------------------

std::string eos_encoded_path(const std::string& abs_path) {
  return "/#curl#" + curl_escape(abs_path);
}

std::string eos_request_path(const std::string& abs_path) {
  return eos_encoded_path(abs_path) + "?eos.encodepath=1";
}

std::string symlink_request(const std::string& abs_path, const std::string& target) {
  std::string metadata;
  put_bytes(metadata, 1, abs_path);  // Metadata.path
  std::string symlink;
  put_bytes(symlink, 1, target);  // FileSymlinkProto.target_path
  put_bool(symlink, 2, true);     // FileSymlinkProto.force
  std::string file;
  put_bytes(file, 1, metadata);  // FileProto.md
  put_bytes(file, 12, symlink);  // FileProto.symlink
  std::string request;
  put_bytes(request, 29, file);  // RequestProto.file
  return request;
}

bool symlink_safe(std::string_view abs_path, std::string_view target) {
  if (abs_path.find("#AND#") != std::string_view::npos) return false;
  if (target.find('\n') != std::string_view::npos) return false;
  for (std::string_view id : {"fid:", "fxid:", "cid:", "cxid:"})
    if (target.starts_with(id)) return false;
  return true;
}

EosEndpoint::ProcResult parse_proc_reply(const std::string& reply) {
  constexpr std::string_view kOut = "mgm.proc.stdout=";
  constexpr std::string_view kErr = "&mgm.proc.stderr=";
  constexpr std::string_view kRetc = "&mgm.proc.retc=";
  EosEndpoint::ProcResult result;
  auto out_pos = reply.find(kOut);
  auto retc_pos = reply.rfind(kRetc);
  if (out_pos == std::string::npos || retc_pos == std::string::npos ||
      retc_pos < out_pos + kOut.size()) {
    result.out = reply;
    return result;
  }
  size_t out_begin = out_pos + kOut.size();
  size_t out_end = retc_pos;
  auto err_pos = reply.rfind(kErr, retc_pos);
  if (err_pos != std::string::npos && err_pos >= out_begin) {
    out_end = err_pos;
    result.err = reply.substr(err_pos + kErr.size(), retc_pos - err_pos - kErr.size());
    while (!result.err.empty() && result.err.back() == '\n') result.err.pop_back();
  }
  result.out = reply.substr(out_begin, out_end - out_begin);
  result.retc = std::atoi(reply.c_str() + retc_pos + kRetc.size());
  return result;
}

Result<std::optional<Entry>> parse_find_line(std::string_view line, std::string_view abs_dir) {
  constexpr std::string_view kPath = "path=\"";
  constexpr std::string_view kPathEnd = "\" type=";
  auto bad = [&](std::string_view problem) {
    return Error{ErrorKind::Other, std::string(problem) + ": " + std::string(line)};
  };
  // The path and the symlink target are printed raw, so a name or target
  // that contains the end marker of the path leaves two ways to split.
  auto path_end = line.find(kPathEnd, kPath.size());
  if (!line.starts_with(kPath) || path_end == std::string_view::npos)
    return bad("unparsable listing line");
  if (line.find(kPathEnd, path_end + 1) != std::string_view::npos)
    return bad("ambiguous listing line");
  std::string_view path = line.substr(kPath.size(), path_end - kPath.size());

  // "type=... size=N uid=U gid=G mode=M flags=F mtime=S.N target="...""; the
  // target comes last.
  Entry e;
  std::string_view rest = line.substr(path_end + 2);
  while (!rest.empty()) {
    if (rest.front() == ' ') {
      rest.remove_prefix(1);
      continue;
    }
    auto eq = rest.find('=');
    if (eq == std::string_view::npos) return bad("unparsable listing line");
    std::string_view key = rest.substr(0, eq);
    rest.remove_prefix(eq + 1);
    std::string_view value;
    if (key == "target") {
      if (rest.size() < 2 || rest.front() != '"' || rest.back() != '"')
        return bad("unparsable listing line");
      value = rest.substr(1, rest.size() - 2);
      rest = {};
    } else {
      value = rest.substr(0, rest.find(' '));
      rest.remove_prefix(value.size());
    }
    if (key == "type") {
      if (value == "directory")
        e.type = EntryType::Directory;
      else if (value == "symlink")
        e.type = EntryType::Symlink;
      else if (value == "file")
        e.type = EntryType::File;
      else
        return bad("unknown type in listing line");
    } else if (key == "size") {
      e.size = parse_number<uint64_t>(value);
    } else if (key == "uid") {
      e.uid = parse_number<uint32_t>(value);
    } else if (key == "gid") {
      e.gid = parse_number<uint32_t>(value);
    } else if (key == "mode" || key == "flags") {
      e.mode = parse_number<ModeBits>(value, 8) & 07777;
    } else if (key == "mtime") {
      e.mtime = parse_timespec(value);
    } else if (key == "target") {
      e.link_target = std::string(value);
    }
  }
  if (e.type == EntryType::Symlink) {
    e.mode = 0777;
    e.size = 0;
  }
  if (e.type == EntryType::Directory) {
    e.size = 0;
    if (path.size() > 1 && path.back() == '/') path.remove_suffix(1);
  }

  if (path == abs_dir) return std::optional<Entry>();
  std::string prefix = abs_dir == "/" ? "/" : std::string(abs_dir) + "/";
  if (!path.starts_with(prefix)) return bad("listing line outside the directory");
  std::string_view name = path.substr(prefix.size());
  bool control = std::any_of(name.begin(), name.end(), [](char ch) {
    auto c = static_cast<unsigned char>(ch);
    return c < 0x20 || c == 0x7f;
  });
  if (!valid_entry_name(name) || control) return bad("invalid name in listing line");
  e.name = std::string(name);
  return std::optional<Entry>(std::move(e));
}

std::optional<std::vector<std::string>> parse_find_denials(std::string_view err,
                                                           std::string_view abs_dir) {
  // "error(13): no permissions to read directory <path>/" or
  // "error(13): public access level restriction on directory <path>/"
  constexpr std::string_view kPrefixes[] = {"error(13): no permissions to read directory ",
                                            "error(13): public access level restriction on "
                                            "directory "};
  std::string prefix = abs_dir == "/" ? "/" : std::string(abs_dir) + "/";
  std::vector<std::string> names;
  while (!err.empty()) {
    std::string_view line = err.substr(0, err.find('\n'));
    err.remove_prefix(std::min(err.size(), line.size() + 1));
    if (line.empty()) continue;
    auto it = std::find_if(std::begin(kPrefixes), std::end(kPrefixes),
                           [&](std::string_view p) { return line.starts_with(p); });
    if (it == std::end(kPrefixes)) return std::nullopt;
    std::string_view path = line.substr(it->size());
    if (path.size() > 1 && path.back() == '/') path.remove_suffix(1);
    if (!path.starts_with(prefix)) return std::nullopt;
    std::string_view name = path.substr(prefix.size());
    if (!valid_entry_name(name)) return std::nullopt;
    names.emplace_back(name);
  }
  if (names.empty()) return std::nullopt;
  return names;
}

// ---- EosEndpoint ------------------------------------------------------------------------

EosEndpoint::EosEndpoint(const std::string& url, EndpointUrl parts, XrdOptions options)
    : XrdEndpoint(url, std::move(parts), options) {
  caps_.mtime_resolution = 1;
  caps_.has_owners = true;
  caps_.can_set_mtime = true;
  caps_.has_symlinks = true;
  caps_.symlink_owner = false;  // settled by probe_identity()
  caps_.can_set_mode = true;
  // EOS keeps the permission bits of files, and clears setuid on directories
  // (EOS 5.5 mgm/ofs/cmds/Chmod.inc).
  caps_.file_mode_bits = 0777;
  caps_.dir_mode_bits = 03777;
  caps_.checksum = ChecksumType::Adler32;
}

Result<std::unique_ptr<EosEndpoint>> EosEndpoint::create(const std::string& url,
                                                         XrdOptions options) {
  auto parts = parse_endpoint_url(url);
  if (!parts.ok()) return parts.error();
  std::unique_ptr<EosEndpoint> ep(new EosEndpoint(url, std::move(parts).value(), options));
  Status probed = ep->probe_identity();
  if (!probed.ok()) return probed.error();
  auto real = ep->resolve(ep->root_);
  if (!real.ok()) return real.error();
  if (real.value() != ep->root_) {
    log::debug(ep->describe(), " is ", real.value());
    ep->root_ = std::move(real).value();
  }
  return ep;
}

Result<bool> EosEndpoint::is_eos(const std::string& url) {
  auto parts = parse_endpoint_url(url);
  if (!parts.ok()) return parts.error();
  std::unique_ptr<EosEndpoint> ep(new EosEndpoint(url, std::move(parts).value(), {}));
  bool answered = true;
  auto result = ep->run_proc("mgm.cmd=whoami", &answered);
  if (!result.ok()) {
    // A server that refuses the command is no MGM; one that cannot be
    // reached may still be one.
    if (!answered) return result.error();
    log::debug(display_url(url), ": ", result.error().describe());
    return false;
  }
  return result.value().out.find("Virtual Identity: ") != std::string::npos;
}

// Finds out who EOS takes us for. Only root may set owners: sudoers can set
// neither the owner of directories nor the group of files, and modes only on
// entries they own (EOS 5.5 mgm/ofs/cmds/Chown.inc and Chmod.inc).
Status EosEndpoint::probe_identity() {
  auto result = proc("mgm.cmd=whoami");
  if (!result.ok()) return result.error();
  const std::string& out = result.value().out;
  constexpr std::string_view kUid = "Virtual Identity: uid=";
  auto uid = out.find(kUid);
  if (uid == std::string::npos)
    return Error{ErrorKind::Other, url_ + " is not an EOS instance (whoami: " + out + ")"};
  bool root = out.compare(uid + kUid.size(), 2, "0 ") == 0;
  caps_.can_set_owner = root;
  caps_.symlink_owner = root;
  log::debug(url_, ": ", out);
  if (!root && out.find(" sudo*") != std::string::npos)
    log::debug(url_, ": a sudoer, but EOS lets only root set owners");
  return {};
}

Result<EosEndpoint::ProcResult> EosEndpoint::proc(const std::string& query) {
  return run_proc(query, nullptr);
}

Result<EosEndpoint::ProcResult> EosEndpoint::run_proc(const std::string& query, bool* answered) {
  std::string url = with_cgi(server_ + "//proc/user/?" + query + std::string(kApp), cgi_);
  auto failed = [&](const XrdCl::XRootDStatus& st, const std::string& context) {
    if (answered) *answered = st.code == XrdCl::errErrorResponse;
    return xrd_error(st, context + display_url(server_));
  };
  XrdCl::File file;
  XrdCl::XRootDStatus st = file.Open(url, XrdCl::OpenFlags::Read);
  if (!st.IsOK()) return failed(st, "command on ");
  std::string response;
  std::vector<char> buf(1 << 20);
  uint64_t offset = 0;
  for (;;) {
    uint32_t got = 0;
    st = file.Read(offset, static_cast<uint32_t>(buf.size()), buf.data(), got);
    if (!st.IsOK()) {
      XrdCl::XRootDStatus closed = file.Close();
      (void)closed;
      return failed(st, "command response from ");
    }
    if (got == 0) break;
    response.append(buf.data(), got);
    offset += got;
  }
  st = file.Close();
  (void)st;
  return parse_proc_reply(response);
}

bool EosEndpoint::is_temporary(std::string_view name) const {
  return XrdEndpoint::is_temporary(name) || name.rfind(".sys.a#.", 0) == 0;
}

Result<std::string> EosEndpoint::request_path(const std::string& abs_path) const {
  return with_cgi(eos_request_path(abs_path), cgi_);
}

Result<Entry> EosEndpoint::stat(const RelPath& path) {
  auto e = stat_abs(absolute(path));
  if (e.ok() && path.empty()) e.value().name.clear();
  return e;
}

Result<Entry> EosEndpoint::stat_abs(const std::string& abs) {
  std::string request = request_path(abs).value();
  XrdCl::Buffer arg;
  arg.FromString(request + "&mgm.pcmd=stat" + std::string(kApp));
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
  e.name = name_of(abs);
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
    link_arg.FromString(request + "&mgm.pcmd=readlink" + std::string(kApp));
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

Result<std::string> EosEndpoint::resolve(const std::string& abs_path) {
  std::vector<std::string> pending;  // the components still to resolve, the next one last
  auto push = [&](std::string_view path) {
    std::vector<std::string> parts;
    for (size_t i = 0; i < path.size();) {
      size_t slash = std::min(path.find('/', i), path.size());
      if (slash > i) parts.emplace_back(path.substr(i, slash - i));
      i = slash + 1;
    }
    pending.insert(pending.end(), parts.rbegin(), parts.rend());
  };
  push(abs_path);
  std::string resolved;  // "" for the root
  bool beyond = false;   // past a part that cannot be resolved
  int links = 0;
  while (!pending.empty()) {
    std::string part = std::move(pending.back());
    pending.pop_back();
    if (part == ".") continue;
    if (part == "..") {
      resolved = parent_of(resolved);
      continue;
    }
    std::string next = resolved + "/" + part;
    if (!beyond) {
      auto st = stat_abs(next);
      if (!st.ok()) {
        ErrorKind kind = st.error().kind;
        if (kind != ErrorKind::NotFound && kind != ErrorKind::Permission) return st.error();
        beyond = true;
      } else if (st.value().type == EntryType::Symlink) {
        if (++links > 40) return errno_error(ELOOP, "resolve " + url_of(abs_path));
        const std::string& target = st.value().link_target;
        if (target.starts_with('/')) resolved.clear();
        push(target);
        continue;
      }
    }
    resolved = std::move(next);
  }
  return resolved.empty() ? std::string("/") : resolved;
}

Result<std::vector<Entry>> EosEndpoint::list(const RelPath& dir) {
  std::string abs = absolute(dir);
  // The link target comes last: parse_find_line takes it to the end of the line.
  std::string request = find_request(abs, "type,size,uid,gid,mode,flags,mtime,link");
  auto result = proc("mgm.cmd.proto=" + base64(request));
  if (!result.ok()) return result.error();
  const ProcResult& reply = result.value();
  // For identities other than root and sudoers, find stops after 100000
  // files (counting those of the subdirectories) or 50000 directories
  // (E2BIG), and leaves out subdirectories they may not read (EACCES),
  // naming them in its error output (EOS 5.5 mgm/proc/user/NewfindCmd.cc).
  bool truncated = reply.retc == E2BIG;
  std::vector<std::string> denied;
  if (reply.retc == EACCES) {
    auto names = parse_find_denials(reply.err, abs);
    if (!names) return errno_error(EACCES, "list " + url_of(abs) + ": " + reply.err);
    denied = std::move(*names);
  } else if (reply.retc != 0 && !truncated) {
    return errno_error(reply.retc, "list " + url_of(abs) + ": " + reply.err);
  }

  std::vector<Entry> entries;
  bool listed_itself = false;
  size_t skipped = 0;
  std::string first_problem;
  std::istringstream lines(reply.out);
  std::string line;
  while (std::getline(lines, line)) {
    if (line.empty()) continue;
    auto parsed = parse_find_line(line, abs);
    if (!parsed.ok()) {
      if (skipped++ == 0) first_problem = parsed.error().message;
      continue;
    }
    if (parsed.value())
      entries.push_back(std::move(*parsed.value()));
    else
      listed_itself = true;
  }
  // find prints paths below the directory's real path, which the root is.
  if (!listed_itself && !truncated)
    return Error{ErrorKind::Other,
                 "the listing of " + url_of(abs) + " does not name the directory itself"};
  // A name listed twice means that a name with line breaks forged a line:
  // neither entry can be trusted.
  std::unordered_map<std::string, int> listed;
  for (const Entry& e : entries) ++listed[e.name];
  skipped += std::erase_if(entries, [&](const Entry& e) {
    if (listed[e.name] == 1) return false;
    if (first_problem.empty()) first_problem = "listed more than once: " + e.name;
    return true;
  });
  if (skipped > 0)
    log::warn("skipping ", skipped, " entries of ", url_of(abs),
              " that cannot be listed safely, the first: ", first_problem);
  if (truncated) {
    Status completed = complete_listing(abs, entries);
    if (!completed.ok()) return completed.error();
  }
  // Subdirectories left out for lack of permission are listed all the same,
  // so that walking into them fails for them alone.
  for (const std::string& name : denied) {
    bool known = std::any_of(entries.begin(), entries.end(),
                             [&](const Entry& e) { return e.name == name; });
    if (known) continue;
    auto e = stat_abs(abs == "/" ? "/" + name : abs + "/" + name);
    if (!e.ok()) {
      if (e.error().kind == ErrorKind::NotFound) continue;
      return e.error();
    }
    entries.push_back(std::move(e).value());
  }
  return entries;
}

Status EosEndpoint::complete_listing(const std::string& abs, std::vector<Entry>& entries) {
  if (!truncation_warned_.exchange(true))
    log::warn("EOS cuts find results short for this identity: large directories of ",
              describe(), " are listed entry by entry, which is slow");
  auto request = request_path(abs);
  XrdCl::DirectoryList* listing = nullptr;
  XrdCl::XRootDStatus st = fs_->DirList(request.value(), XrdCl::DirListFlags::None, listing);
  if (!st.IsOK()) return xrd_error(st, "list " + url_of(abs));
  std::unique_ptr<XrdCl::DirectoryList> owned(listing);
  std::unordered_set<std::string> names;
  for (auto it = listing->Begin(); it != listing->End(); ++it) names.insert((*it)->GetName());
  // A line of the cut listing for a name that does not exist was forged.
  std::erase_if(entries, [&](const Entry& e) { return !names.count(e.name); });
  std::unordered_set<std::string> listed;
  for (const Entry& e : entries) listed.insert(e.name);
  for (const std::string& name : names) {
    // find skips version directories, and so does this.
    if (listed.count(name) || !valid_entry_name(name) || name.starts_with(".sys.v#.")) continue;
    auto e = stat_abs(abs == "/" ? "/" + name : abs + "/" + name);
    if (!e.ok()) {
      if (e.error().kind == ErrorKind::NotFound) continue;
      return e.error();
    }
    entries.push_back(std::move(e).value());
  }
  return {};
}

Status EosEndpoint::symlink(const RelPath& path, const std::string& target) {
  std::string abs = absolute(path);
  if (!symlink_safe(abs, target))
    return Error{ErrorKind::Unsupported,
                 "symlink " + url_of(abs) + ": EOS cannot store this path or target as it is"};
  // The protobuf command takes path and target as bytes, and replaces an
  // existing symlink.
  auto result = proc("mgm.cmd.proto=" + base64(symlink_request(abs, target)));
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
  std::string request = request_path(abs).value();
  // The mtime first: utimes needs write access, which the mode may take away.
  if (has(fields, MetaFields::Mtime)) {
    char nsec[16];
    std::snprintf(nsec, sizeof nsec, "%09d", md.mtime.nsec);
    XrdCl::Buffer arg;
    arg.FromString(request + "&mgm.pcmd=utimes&tv1_sec=0&tv1_nsec=0&tv2_sec=" +
                   std::to_string(md.mtime.sec) + "&tv2_nsec=" + nsec + std::string(kApp));
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
  // chown with option h changes a symlink itself rather than its target.
  if (has(fields, MetaFields::Owner) && (!is_symlink || caps_.symlink_owner)) {
    auto result = proc("mgm.cmd=chown&mgm.chown.option=h&mgm.path=" + eos_encoded_path(abs) +
                       "&eos.encodepath=1&mgm.chown.owner=" + std::to_string(md.uid) + ":" +
                       std::to_string(md.gid));
    if (!result.ok()) return result.error();
    if (result.value().retc != 0)
      return errno_error(result.value().retc, "chown " + url_of(abs) + ": " + result.value().err);
  }
  if (has(fields, MetaFields::Mode) && !is_symlink) {
    char mode[8];
    std::snprintf(mode, sizeof mode, "%o", md.mode & 07777);
    auto result = proc("mgm.cmd=chmod&mgm.path=" + eos_encoded_path(abs) + "&eos.encodepath=1" +
                       "&mgm.chmod.mode=" + mode);
    if (!result.ok()) return result.error();
    if (result.value().retc != 0)
      return errno_error(result.value().retc, "chmod " + url_of(abs) + ": " + result.value().err);
  }
  return {};
}

Result<std::unique_ptr<FileReader>> EosEndpoint::open_read(const RelPath& path) {
  auto inner = XrdEndpoint::open_read(path);
  if (!inner.ok()) return inner.error();
  return std::unique_ptr<FileReader>(new EosReader(*this, std::move(inner).value(), path));
}

Result<std::unique_ptr<FileWriter>> EosEndpoint::open_write(const RelPath& path,
                                                            const CommitSpec& spec) {
  std::string abs = absolute(path);
  std::string url = open_url(request_path(abs).value() + "&eos.atomic=1" + std::string(kApp));
  if (has(spec.fields, MetaFields::Mtime)) url += "&eos.mtime=" + format_timespec(spec.metadata.mtime);
  auto file = new_write_file();
  ModeBits mode = has(spec.fields, MetaFields::Mode) ? spec.metadata.mode : 0644;
  XrdCl::XRootDStatus st = file->Open(url, XrdCl::OpenFlags::Delete | XrdCl::OpenFlags::Write,
                                      static_cast<XrdCl::Access::Mode>(mode & 0777));
  if (!st.IsOK()) return xrd_error(st, "create " + url_of(abs));
  std::unique_ptr<FileWriter> upload(new EosUploadWriter(std::move(file), url_of(abs)));
  return std::unique_ptr<FileWriter>(new EosWriter(*this, std::move(upload), abs, url_of(abs)));
}

}  // namespace eosmirror
