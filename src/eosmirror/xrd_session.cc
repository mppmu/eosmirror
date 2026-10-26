// SPDX-License-Identifier: GPL-3.0-or-later
#include "eosmirror/xrd_session.hh"

#include <XrdCl/XrdClFile.hh>
#include <XrdCl/XrdClFileSystem.hh>
#include <XrdCl/XrdClStatus.hh>
#include <XrdCl/XrdClURL.hh>
#include <XrdCl/XrdClXRootDResponses.hh>
#include <XProtocol/XProtocol.hh>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <future>
#include <mutex>
#include <sstream>
#include <unordered_map>
#include <vector>

#include "eosmirror/log.hh"
#include "eosmirror/xrd_client.hh"

namespace eosmirror {

namespace {

std::string parent_of(const std::string& path) {
  auto slash = path.rfind('/');
  return slash == std::string::npos ? "" : path.substr(0, slash);
}

std::string name_of(const std::string& path) {
  auto slash = path.rfind('/');
  return slash == std::string::npos ? path : path.substr(slash + 1);
}

XrdCl::Access::Mode access_mode(ModeBits mode) {
  return static_cast<XrdCl::Access::Mode>(mode & 0777);
}

Entry entry_from_stat(std::string name, const XrdCl::StatInfo& info) {
  Entry e;
  e.name = std::move(name);
  if (info.TestFlags(XrdCl::StatInfo::IsDir))
    e.type = EntryType::Directory;
  else if (info.TestFlags(XrdCl::StatInfo::Other))
    e.type = EntryType::Other;
  else
    e.type = EntryType::File;
  if (e.type == EntryType::File) e.size = info.GetSize();
  if (e.type == EntryType::Directory) e.id = info.GetId();  // device and inode
  e.mtime = {static_cast<int64_t>(info.GetModTime()), 0};
  if (info.ExtendedFormat()) {
    e.ctime = {static_cast<int64_t>(info.GetChangeTime()), 0};
    // The raw octal string from the server ("0640"); GetModeAsOctString()
    // would render it symbolically despite its name.
    std::string oct = info.GetModeAsString();
    e.mode = static_cast<ModeBits>(std::strtoul(oct.c_str(), nullptr, 8)) & 07777;
  }
  return e;
}

// A checksum reply, "<type> <hex>". EOS replies "none" for files without a
// checksum. Those, and types that cannot be computed here, come back as None.
Result<Checksum> parse_checksum(const std::string& text, const std::string& context) {
  std::istringstream in(text);
  std::string type, value;
  in >> type >> value;
  if (type.empty())
    return Error{ErrorKind::Other, "unexpected checksum response for " + context + ": " + text};
  auto parsed = parse_checksum_type(type);
  if (!parsed || *parsed == ChecksumType::None) {
    if (!parsed) log::debug(context, " has a checksum of type ", type);
    return Checksum{};
  }
  if (value.empty())
    return Error{ErrorKind::Other, "unexpected checksum response for " + context + ": " + text};
  for (auto& c : value) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return Checksum{*parsed, value};
}

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

// The calling thread's number, which names its own connections.
int thread_number() {
  static std::atomic<int> threads{0};
  thread_local int number = threads++;
  return number;
}

// An XrdCl response handler that passes the status and response to a
// function, from XrdCl's thread, and deletes itself.
class Callback : public XrdCl::ResponseHandler {
 public:
  using Function = std::function<void(const XrdCl::XRootDStatus&, XrdCl::AnyObject*)>;
  explicit Callback(Function f) : f_(std::move(f)) {}

  void HandleResponse(XrdCl::XRootDStatus* status, XrdCl::AnyObject* response) override {
    std::unique_ptr<XrdCl::XRootDStatus> owned_status(status);
    std::unique_ptr<XrdCl::AnyObject> owned_response(response);
    Function f = std::move(f_);
    delete this;
    f(*owned_status, owned_response.get());
  }

 private:
  Function f_;
};

}  // namespace

Result<EndpointUrl> parse_endpoint_url(const std::string& url) {
  XrdCl::URL parsed(url);
  if (!parsed.IsValid() || parsed.GetProtocol().empty() || parsed.GetHostName().empty())
    return Error{ErrorKind::Other, "invalid URL: " + display_url(url)};
  EndpointUrl parts;
  parts.server = parsed.GetProtocol() + "://" + parsed.GetHostId();
  parts.host = parsed.GetHostName() + ":" + std::to_string(parsed.GetPort());
  for (auto& c : parts.host) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  parts.path = parsed.GetPath();
  while (parts.path.size() > 1 && parts.path.back() == '/') parts.path.pop_back();
  if (parts.path.empty() || parts.path[0] != '/')
    return Error{ErrorKind::Other, "the URL needs an absolute path: " + display_url(url)};
  // Servers resolve '.' and '..' after following symlinks, which comparing
  // roots cannot.
  for (std::string_view rest = parts.path; !rest.empty();) {
    std::string_view part = rest.substr(0, rest.find('/'));
    rest.remove_prefix(std::min(rest.size(), part.size() + 1));
    if (part == "." || part == "..")
      return Error{ErrorKind::Other, "the URL's path must not contain '.' or '..': " +
                                         display_url(url)};
  }
  if (auto query = url.find('?'); query != std::string::npos) parts.cgi = url.substr(query + 1);
  return parts;
}

std::string with_cgi(std::string request, std::string_view cgi) {
  if (cgi.empty()) return request;
  request += request.find('?') == std::string::npos ? '?' : '&';
  request += cgi;
  return request;
}

Status move_replacing(const std::string& from, const std::string& to,
                      const std::function<Status(const std::string&, const std::string&)>& move,
                      const std::function<Status(const std::string&)>& remove) {
  Status moved = move(from, to);
  if (moved.ok() || moved.error().kind != ErrorKind::Exists) return moved;
  std::string aside = parent_of(to) + "/" + temporary_name(name_of(to));
  if (Status s = move(to, aside); !s.ok()) return s;
  moved = move(from, to);
  if (!moved.ok()) {
    if (Status back = move(aside, to); !back.ok())
      log::error("cannot move ", aside, " back to ", to, " after a failed rename: ",
                 back.error().describe());
    return moved;
  }
  // Left over, it is a temporary that a later run with --delete removes.
  if (Status removed = remove(aside); !removed.ok())
    log::warn("cannot remove the replaced ", aside, ": ", removed.error().describe());
  return {};
}

Result<std::string> xrootd_request_path(const std::string& abs_path) {
  if (abs_path.find('?') != std::string::npos)
    return Error{ErrorKind::Unsupported,
                 "XRootD cannot address a path containing '?': " + abs_path};
  return abs_path;
}

std::string eos_encoded_path(const std::string& abs_path) {
  return "/#curl#" + curl_escape(abs_path);
}

std::string eos_request_path(const std::string& abs_path) {
  return eos_encoded_path(abs_path) + "?eos.encodepath=1" + std::string(kEosApp);
}

Error xrd_error(const XrdCl::XRootDStatus& st, const std::string& context) {
  // A server error carries the protocol's kXR_* code in errNo, a local one
  // an errno.
  if (st.code == XrdCl::errErrorResponse || st.code == XrdCl::errLocalError ||
      st.code == XrdCl::errOSError) {
    int err = static_cast<int>(st.errNo);
    bool busy = false;
    if (err >= kXR_ArgInvalid) {
      // Overloaded or failing servers. kXR_FSError is what servers send for
      // errnos without a protocol code, such as EAGAIN, EBUSY and ESTALE.
      busy = err == kXR_Overloaded || err == kXR_ServerError || err == kXR_noReplicas ||
             err == kXR_inProgress || err == kXR_FSError;
      err = XProtocol::toErrno(err);
    }
    Error e = errno_error(err, context);
    if (busy) e.kind = ErrorKind::IO;
    if (!st.GetErrorMessage().empty()) e.message += " (" + st.GetErrorMessage() + ")";
    return e;
  }
  ErrorKind kind;
  switch (st.code) {
    case XrdCl::errNotFound: kind = ErrorKind::NotFound; break;
    case XrdCl::errAuthFailed:
    case XrdCl::errLoginFailed: kind = ErrorKind::Permission; break;
    case XrdCl::errNotSupported:
    case XrdCl::errNotImplemented:
    case XrdCl::errQueryNotSupported: kind = ErrorKind::Unsupported; break;
    case XrdCl::errOperationExpired:
    case XrdCl::errSocketTimeout: kind = ErrorKind::Timeout; break;
    case XrdCl::errRetry:
    case XrdCl::errInvalidAddr:
    case XrdCl::errTlsError:
    case XrdCl::errHandShakeFailed:
    case XrdCl::errNoMoreFreeSIDs:
    case XrdCl::errSocketError:
    case XrdCl::errSocketDisconnected:
    case XrdCl::errStreamDisconnect:
    case XrdCl::errConnectionError:
    case XrdCl::errInvalidSession:
    case XrdCl::errNoMoreReplicas:
    case XrdCl::errOperationInterrupted:
    case XrdCl::errInvalidResponse:
    case XrdCl::errInvalidMessage: kind = ErrorKind::IO; break;
    case XrdCl::errDataError: kind = ErrorKind::Checksum; break;
    default: kind = ErrorKind::Other; break;
  }
  return Error{kind, context + ": " + st.ToStr()};
}

// ---- XrdSession ----------------------------------------------------------------------

XrdSession::XrdSession(const std::string& url, EndpointUrl parts, XrdOptions options,
                       PathEncoding encoding, std::shared_ptr<XrdClient> client)
    : url_(display_url(url)),
      server_(std::move(parts.server)),
      host_(std::move(parts.host)),
      root_(std::move(parts.path)),
      cgi_(std::move(parts.cgi)),
      options_(options),
      encoding_(encoding),
      client_(client ? std::move(client) : xrdcl_client()) {}

XrdSession::~XrdSession() = default;

std::string XrdSession::open_url(const std::string& request_path) const {
  std::string url = server_ + "/" + request_path;
  if (options_.shared_connection) return url;
  // XrdCl keys its connections by host and this parameter (URL::GetChannelId,
  // XrdClURL.cc), keeps it for redirections and does not send it to servers.
  return with_cgi(std::move(url), "xrdcl.intent=eosmirror" + std::to_string(thread_number()));
}

XrdFsApi& XrdSession::fs() {
  int thread = options_.shared_connection ? -1 : thread_number();
  std::lock_guard lock(fs_mutex_);
  std::unique_ptr<XrdFsApi>& slot = fs_[thread];
  // The URL's parameters, such as xrd.wantprot, go with the login.
  if (!slot) slot = client_->new_file_system(with_cgi(open_url(""), cgi_));
  return *slot;
}

Location XrdSession::location() const {
  std::string path = std::filesystem::path(root_).lexically_normal().string();
  while (path.size() > 1 && path.back() == '/') path.pop_back();
  return {host_, path};
}

std::string XrdSession::absolute(const RelPath& path) const {
  if (path.empty()) return root_;
  return root_ == "/" ? "/" + path : root_ + "/" + path;
}

std::string XrdSession::url_of(const std::string& abs_path) const {
  return display_url(server_) + "/" + abs_path;
}

Result<std::string> XrdSession::request_path(const std::string& abs_path) const {
  if (encoding_ == PathEncoding::Eos) return with_cgi(eos_request_path(abs_path), cgi_);
  auto request = xrootd_request_path(abs_path);
  if (!request.ok()) return request.error();
  return with_cgi(std::move(request).value(), cgi_);
}

// A file for writing, with XrdCl's write recovery off: after a dropped
// connection, recovery reopens the file and resends only the pending
// writes, which can never complete an upload that the server verifies.
// Failing fast and copying the file again is the right recovery.
std::unique_ptr<XrdFileApi> XrdSession::new_write_file() {
  auto file = new_file();
  file->set_property("WriteRecovery", "false");
  return file;
}

std::unique_ptr<XrdFileApi> XrdSession::new_file() {
  return client_->new_file();
}

Result<std::unique_ptr<XrdFileApi>> XrdSession::open_file(const std::string& abs) {
  auto request = request_path(abs);
  if (!request.ok()) return request.error();
  auto file = new_file();
  XrdCl::XRootDStatus st = file->open(open_url(request.value()), XrdCl::OpenFlags::Read);
  if (!st.IsOK()) return xrd_error(st, "open " + url_of(abs));
  return file;
}

Result<Entry> XrdSession::stat_abs(const std::string& abs) {
  auto request = request_path(abs);
  if (!request.ok()) return request.error();
  XrdCl::StatInfo* info = nullptr;
  XrdCl::XRootDStatus st = fs().stat(request.value(), info);
  if (!st.IsOK()) return xrd_error(st, "stat " + url_of(abs));
  std::unique_ptr<XrdCl::StatInfo> owned(info);
  return entry_from_stat(name_of(abs), *info);
}

Status XrdSession::mkdir_abs(const std::string& abs, ModeBits mode) {
  auto request = request_path(abs);
  if (!request.ok()) return request.error();
  XrdCl::XRootDStatus st =
      fs().mkdir(request.value(), XrdCl::MkDirFlags::None, access_mode(mode | 0700));
  if (!st.IsOK()) return xrd_error(st, "mkdir " + url_of(abs));
  return {};
}

Status XrdSession::chmod_abs(const std::string& abs, ModeBits mode) {
  auto request = request_path(abs);
  if (!request.ok()) return request.error();
  XrdCl::XRootDStatus st = fs().chmod(request.value(), access_mode(mode));
  if (!st.IsOK()) return xrd_error(st, "chmod " + url_of(abs));
  return {};
}

Status XrdSession::rmdir_abs(const std::string& abs) {
  auto request = request_path(abs);
  if (!request.ok()) return request.error();
  XrdCl::XRootDStatus st = fs().rmdir(request.value());
  if (!st.IsOK()) return xrd_error(st, "rmdir " + url_of(abs));
  return {};
}

Status XrdSession::remove_abs(const std::string& abs) {
  auto request = request_path(abs);
  if (!request.ok()) return request.error();
  XrdCl::XRootDStatus st = fs().rm(request.value());
  if (!st.IsOK()) return xrd_error(st, "remove " + url_of(abs));
  return {};
}

Status XrdSession::move_abs(const std::string& from, const std::string& to) {
  auto request_from = request_path(from);
  if (!request_from.ok()) return request_from.error();
  auto request_to = request_path(to);
  if (!request_to.ok()) return request_to.error();
  XrdCl::XRootDStatus st = fs().mv(request_from.value(), request_to.value());
  if (!st.IsOK()) return xrd_error(st, "rename " + url_of(from) + " to " + url_of(to));
  return {};
}

Status XrdSession::rename_abs(const std::string& from, const std::string& to) {
  return move_replacing(
      from, to, [&](const std::string& a, const std::string& b) { return move_abs(a, b); },
      [&](const std::string& abs) { return remove_abs(abs); });
}

Result<Checksum> XrdSession::query_checksum(const std::string& abs) {
  auto request = request_path(abs);
  if (!request.ok()) return request.error();
  XrdCl::Buffer arg;
  arg.FromString(request.value());
  XrdCl::Buffer* response = nullptr;
  XrdCl::XRootDStatus st = fs().query(XrdCl::QueryCode::Checksum, arg, response);
  if (!st.IsOK()) return xrd_error(st, "checksum of " + url_of(abs));
  std::unique_ptr<XrdCl::Buffer> owned(response);
  return parse_checksum(response ? response->ToString() : "", url_of(abs));
}

// A server ends a listing with stat information at the first entry it cannot
// stat for a reason other than ENOENT, such as a symlink loop, and reports
// success (XrdOssDir::Readdir, XrdOfsDirectory::nextEntry and
// XrdXrootdProtocol::do_DirStat take the error for the end). A listing of
// the names alone stats nothing, so one is taken alongside, and the names it
// has that the other lacks are stated one by one. Names come one per line,
// unescaped: a name with line breaks can forge lines, so names listed more
// than once are left out, and so are lines that are no names ("." or "..",
// which servers do not list), rather than stated.
Result<Listing> XrdSession::list(const RelPath& dir) {
  std::string abs = absolute(dir);
  auto request = request_path(abs);
  if (!request.ok()) return request.error();
  using Names = std::pair<XrdCl::XRootDStatus, std::unique_ptr<XrdCl::DirectoryList>>;
  auto names_listed = std::make_shared<std::promise<Names>>();
  auto names_future = names_listed->get_future();
  auto* handler = new Callback(
      [names_listed](const XrdCl::XRootDStatus& st, XrdCl::AnyObject* response) {
        XrdCl::DirectoryList* names = nullptr;
        if (response) {
          response->Get(names);
          response->Set(static_cast<int*>(nullptr));  // takes it over
        }
        names_listed->set_value({st, std::unique_ptr<XrdCl::DirectoryList>(names)});
      });
  XrdCl::XRootDStatus st = fs().dir_list(request.value(), XrdCl::DirListFlags::None, handler);
  if (!st.IsOK()) {
    delete handler;  // XrdCl calls it only for requests it accepted
    return xrd_error(st, "list " + url_of(abs));
  }
  XrdCl::DirectoryList* listing = nullptr;
  st = fs().dir_list(request.value(), XrdCl::DirListFlags::Stat, listing);
  std::unique_ptr<XrdCl::DirectoryList> owned(listing);
  Names names = names_future.get();
  if (!st.IsOK()) return xrd_error(st, "list " + url_of(abs));
  if (!names.first.IsOK() || !names.second) return xrd_error(names.first, "list " + url_of(abs));

  std::unordered_map<std::string, int> with_stat, named;
  for (auto it = listing->Begin(); it != listing->End(); ++it) ++with_stat[(*it)->GetName()];
  for (auto it = names.second->Begin(); it != names.second->End(); ++it) ++named[(*it)->GetName()];
  auto twice = [](const std::unordered_map<std::string, int>& counts, const std::string& name) {
    auto it = counts.find(name);
    return it != counts.end() && it->second > 1;
  };
  Listing result;
  result.entries.reserve(listing->GetSize());
  size_t forged = 0;
  // An entry stated on its own: one that is gone is no entry, one that
  // cannot be stated is left out.
  auto stat_entry = [&](const std::string& name) {
    if (!valid_entry_name(name)) {
      ++forged;
      return;
    }
    auto e = stat_abs(absolute(join(dir, name)));
    if (e.ok()) {
      result.entries.push_back(std::move(e).value());
    } else if (e.error().kind != ErrorKind::NotFound) {
      log::warn("skipping ", e.error().describe());
      ++result.skipped;
    }
  };
  for (auto it = listing->Begin(); it != listing->End(); ++it) {
    XrdCl::DirectoryList::ListEntry* item = *it;
    if (twice(with_stat, item->GetName()) || twice(named, item->GetName())) {
      ++forged;
    } else if (!item->GetStatInfo()) {
      stat_entry(item->GetName());  // the server lists no stat information
    } else {
      result.entries.push_back(entry_from_stat(item->GetName(), *item->GetStatInfo()));
    }
  }
  for (const auto& [name, count] : named) {
    if (with_stat.count(name)) continue;
    if (count > 1)
      forged += static_cast<size_t>(count);
    else
      stat_entry(name);
  }
  if (forged > 0)
    log::warn("skipping ", forged, " entries of ", url_of(abs),
              " whose names are forged by line breaks");
  result.skipped += forged;
  return result;
}

// ---- XrdWriteFile --------------------------------------------------------------------

XrdWriteFile::XrdWriteFile(std::unique_ptr<XrdFileApi> file, std::string name, int window)
    : file_(std::move(file)),
      name_(std::move(name)),
      writes_(static_cast<size_t>(std::max(window, 1)),
              [this](const Chunk& chunk, WriteWindow::Done done) -> Status {
                auto* handler = new Callback(
                    [this, done = std::move(done)](const XrdCl::XRootDStatus& st,
                                                   XrdCl::AnyObject*) {
                      if (!st.IsOK())
                        return done(upload_error(xrd_error(st, "write " + name_)));
                      done({});
                    });
                XrdCl::XRootDStatus st =
                    file_->write(chunk.offset, static_cast<uint32_t>(chunk.size),
                                 chunk.buffer.span().data(), handler);
                if (!st.IsOK()) {
                  delete handler;  // XrdCl calls it only for writes it accepted
                  return upload_error(xrd_error(st, "write " + name_));
                }
                return {};
              },
              name_) {}

XrdWriteFile::~XrdWriteFile() = default;

Status XrdWriteFile::write(Chunk chunk) {
  if (!file_) return Error{ErrorKind::Other, "write to " + name_ + " after its close"};
  return writes_.write(std::move(chunk));
}

Status XrdWriteFile::complete(uint64_t size) {
  if (!file_) return Error{ErrorKind::Other, "commit of " + name_ + " without an open file"};
  if (Status drained = writes_.drain(); !drained.ok()) return drained;
  if (writes_.written() != size)
    return Error{ErrorKind::Changed, "wrote " + std::to_string(writes_.written()) + " bytes to " +
                                         name_ + ", expected " + std::to_string(size)};
  return {};
}

Status XrdWriteFile::close() {
  XrdCl::XRootDStatus st = file_->close();
  file_.reset();
  if (!st.IsOK())
    return upload_error(writes_.stalled(xrd_error(st, "close " + name_)));
  return {};
}

void XrdWriteFile::drop() {
  file_.reset();
}

void XrdWriteFile::close_quietly() {
  if (!file_) return;
  Status drained = writes_.drain();
  (void)drained;
  XrdCl::XRootDStatus st = file_->close();
  (void)st;
  file_.reset();
}

// ---- XrdReadFile ---------------------------------------------------------------------

XrdReadFile::XrdReadFile(std::unique_ptr<XrdFileApi> file, std::string url, int window)
    : file_(std::move(file)),
      url_(std::move(url)),
      ahead_(static_cast<size_t>(std::max(window, 1)),
             [this](uint64_t offset, std::span<std::byte> buf, ReadAhead::Done done) {
               return submit(offset, buf, std::move(done));
             }) {}

XrdReadFile::~XrdReadFile() {
  ahead_.stop();
  XrdCl::XRootDStatus st = file_->close();
  (void)st;
}

Result<size_t> XrdReadFile::read(uint64_t offset, std::span<std::byte> buf) {
  size_t done = 0;
  while (done < buf.size()) {
    uint32_t got = 0;
    XrdCl::XRootDStatus st = file_->read(offset + done, static_cast<uint32_t>(buf.size() - done),
                                         buf.data() + done, got);
    if (!st.IsOK()) return xrd_error(st, "read " + url_);
    if (got == 0) break;
    done += got;
  }
  return done;
}

Result<Chunk> XrdReadFile::read_chunk(uint64_t offset, uint64_t end, BufferPool& pool) {
  return ahead_.next(offset, end, pool);
}

Result<Entry> XrdReadFile::stat(bool fresh) {
  XrdCl::StatInfo* info = nullptr;
  XrdCl::XRootDStatus st = file_->stat(fresh, info);
  if (!st.IsOK()) return xrd_error(st, "stat " + url_);
  std::unique_ptr<XrdCl::StatInfo> owned(info);
  return entry_from_stat(name_of(url_), *info);
}

Status XrdReadFile::submit(uint64_t offset, std::span<std::byte> buf, ReadAhead::Done done) {
  auto* handler = new Callback([this, done = std::move(done)](const XrdCl::XRootDStatus& st,
                                                              XrdCl::AnyObject* response) {
    if (!st.IsOK()) return done(xrd_error(st, "read " + url_));
    XrdCl::ChunkInfo* info = nullptr;
    if (response) response->Get(info);
    done(info ? size_t{info->GetLength()} : size_t{0});
  });
  XrdCl::XRootDStatus st =
      file_->read(offset, static_cast<uint32_t>(buf.size()), buf.data(), handler);
  if (!st.IsOK()) {
    delete handler;  // XrdCl calls it only for reads it accepted
    return xrd_error(st, "read " + url_);
  }
  return {};
}

}  // namespace eosmirror
