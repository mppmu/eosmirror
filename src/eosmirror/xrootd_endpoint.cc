// SPDX-License-Identifier: GPL-3.0-or-later
#include "eosmirror/xrootd_endpoint.hh"

#include <XrdCl/XrdClFile.hh>
#include <XrdCl/XrdClFileSystem.hh>
#include <XrdCl/XrdClStatus.hh>
#include <XrdCl/XrdClURL.hh>
#include <XrdCl/XrdClXRootDResponses.hh>
#include <XProtocol/XProtocol.hh>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <future>
#include <mutex>
#include <optional>
#include <sstream>
#include <unordered_map>
#include <vector>

#include "eosmirror/log.hh"

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
// checksum; those and types that cannot be computed here come back as None.
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

// Writes to a temporary name; commit() waits for the writes, closes,
// verifies the stored checksum and renames the file into place.
class XrdWriter : public XrdFileWriter {
 public:
  XrdWriter(XrdEndpoint& ep, std::unique_ptr<XrdCl::File> file, std::string temp_abs,
            std::string final_abs, int window)
      : XrdFileWriter(std::move(file), temp_abs, window),
        ep_(ep),
        temp_(std::move(temp_abs)),
        final_(std::move(final_abs)) {}

  ~XrdWriter() override { abort(); }

  Result<CommitInfo> commit(const CommitSpec& spec) override {
    Status s = finish(spec);
    if (!s.ok()) {
      abort();
      return s.error();
    }
    return CommitInfo{verified_};
  }

  void abort() override {
    close_quietly();
    if (!temp_exists_) return;
    temp_exists_ = false;
    Status removed = ep_.remove_abs(temp_);
    if (!removed.ok() && removed.error().kind != ErrorKind::NotFound)
      log::warn("cannot remove temporary ", temp_, ": ", removed.error().describe());
  }

 private:
  Status finish(const CommitSpec& spec) {
    if (Status s = complete(spec.size); !s.ok()) return s;
    if (Status s = close(); !s.ok()) return s;
    if (spec.checksum.type != ChecksumType::None &&
        spec.checksum.type == ep_.capabilities().checksum) {
      auto stored = ep_.query_checksum(temp_);
      if (!stored.ok()) return stored.error();
      if (!(stored.value() == spec.checksum))
        return Error{ErrorKind::Checksum,
                     temp_ + " has checksum " +
                         (stored.value().hex.empty() ? "none" : stored.value().hex) +
                         " instead of " + spec.checksum.hex};
      verified_ = true;
    } else if (spec.require_verification) {
      return Error{ErrorKind::Unsupported, "the server computes no checksum to verify " + temp_ +
                                               " against"};
    } else {
      // Nothing compared: the server must at least have stored all of it.
      auto st = ep_.stat_abs(temp_);
      if (!st.ok()) return st.error();
      if (st.value().size != spec.size)
        return Error{ErrorKind::IO, temp_ + " has " + std::to_string(st.value().size) +
                                        " bytes instead of " + std::to_string(spec.size)};
    }
    Status renamed = ep_.rename_abs(temp_, final_);
    if (renamed.ok()) temp_exists_ = false;
    return renamed;
  }

  XrdEndpoint& ep_;
  std::string temp_;
  std::string final_;
  bool temp_exists_ = true;
  bool verified_ = false;
};

}  // namespace

std::string display_url(std::string_view url) {
  url = url.substr(0, url.find('?'));
  auto scheme = url.find("://");
  size_t host = scheme == std::string_view::npos ? 0 : scheme + 3;
  auto at = url.find('@', host);
  if (at != std::string_view::npos && at < url.find('/', host)) {
    auto colon = url.find(':', host);
    if (colon < at) return std::string(url.substr(0, colon)) + std::string(url.substr(at));
  }
  return std::string(url);
}

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
  // Servers resolve them after symlinks, which comparing roots cannot.
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

Error XrdEndpoint::xrd_error(const XrdCl::XRootDStatus& st, const std::string& context) {
  // A server error carries the protocol's kXR_* code in errNo, a local one
  // an errno.
  if (st.code == XrdCl::errErrorResponse || st.code == XrdCl::errLocalError ||
      st.code == XrdCl::errOSError) {
    int err = static_cast<int>(st.errNo);
    bool busy = false;
    if (err >= kXR_ArgInvalid) {
      // Overloaded or failing servers; kXR_FSError is what servers send for
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

// A file for writing, with XrdCl's write recovery off: after a dropped
// connection, recovery reopens the file and resends only the pending
// writes, which can never complete an upload that the server verifies;
// failing fast and copying the file again is the right recovery.
std::unique_ptr<XrdCl::File> XrdEndpoint::new_write_file() {
  auto file = new_file();
  file->SetProperty("WriteRecovery", "false");
  return file;
}

std::unique_ptr<XrdCl::File> XrdEndpoint::new_file() {
  return std::make_unique<XrdCl::File>(XrdCl::File::DisableVirtRedirect);
}

// ---- XrdFileWriter -------------------------------------------------------------------

XrdFileWriter::XrdFileWriter(std::unique_ptr<XrdCl::File> file, std::string name, int window)
    : file_(std::move(file)),
      name_(std::move(name)),
      writes_(static_cast<size_t>(std::max(window, 1)),
              [this](const Chunk& chunk, WriteWindow::Done done) -> Status {
                auto* handler = new Callback(
                    [this, done = std::move(done)](const XrdCl::XRootDStatus& st,
                                                   XrdCl::AnyObject*) {
                      if (!st.IsOK())
                        return done(upload_error(XrdEndpoint::xrd_error(st, "write " + name_)));
                      done({});
                    });
                XrdCl::XRootDStatus st =
                    file_->Write(chunk.offset, static_cast<uint32_t>(chunk.size),
                                 chunk.buffer.span().data(), handler);
                if (!st.IsOK()) {
                  delete handler;  // XrdCl calls it only for writes it accepted
                  return upload_error(XrdEndpoint::xrd_error(st, "write " + name_));
                }
                return {};
              },
              name_) {}

XrdFileWriter::~XrdFileWriter() = default;

Status XrdFileWriter::write(Chunk chunk) {
  if (!file_) return Error{ErrorKind::Other, "write to " + name_ + " after its close"};
  return writes_.write(std::move(chunk));
}

Status XrdFileWriter::complete(uint64_t size) {
  if (!file_) return Error{ErrorKind::Other, "commit of " + name_ + " without an open file"};
  if (Status drained = writes_.drain(); !drained.ok()) return drained;
  if (writes_.written() != size)
    return Error{ErrorKind::Changed, "wrote " + std::to_string(writes_.written()) + " bytes to " +
                                         name_ + ", expected " + std::to_string(size)};
  return {};
}

Status XrdFileWriter::close() {
  XrdCl::XRootDStatus st = file_->Close();
  file_.reset();
  if (!st.IsOK())
    return upload_error(writes_.stalled(XrdEndpoint::xrd_error(st, "close " + name_)));
  return {};
}

void XrdFileWriter::close_quietly() {
  if (!file_) return;
  Status drained = writes_.drain();
  (void)drained;
  XrdCl::XRootDStatus st = file_->Close();
  (void)st;
  file_.reset();
}

// ---- XrdReader -----------------------------------------------------------------------

XrdReader::XrdReader(std::unique_ptr<XrdCl::File> file, std::string url, int window)
    : file_(std::move(file)),
      url_(std::move(url)),
      ahead_(static_cast<size_t>(std::max(window, 1)),
             [this](uint64_t offset, std::span<std::byte> buf, ReadAhead::Done done) {
               return submit(offset, buf, std::move(done));
             }) {}

XrdReader::~XrdReader() {
  ahead_.stop();
  XrdCl::XRootDStatus st = file_->Close();
  (void)st;
}

Result<size_t> XrdReader::read(uint64_t offset, std::span<std::byte> buf) {
  size_t done = 0;
  while (done < buf.size()) {
    uint32_t got = 0;
    XrdCl::XRootDStatus st = file_->Read(offset + done, static_cast<uint32_t>(buf.size() - done),
                                         buf.data() + done, got);
    if (!st.IsOK()) return XrdEndpoint::xrd_error(st, "read " + url_);
    if (got == 0) break;
    done += got;
  }
  return done;
}

Result<Chunk> XrdReader::read_chunk(uint64_t offset, uint64_t end, BufferPool& pool) {
  return ahead_.next(offset, end, pool);
}

Result<Entry> XrdReader::file_stat(bool fresh) {
  XrdCl::StatInfo* info = nullptr;
  XrdCl::XRootDStatus st = file_->Stat(fresh, info);
  if (!st.IsOK()) return XrdEndpoint::xrd_error(st, "stat " + url_);
  std::unique_ptr<XrdCl::StatInfo> owned(info);
  return entry_from_stat(name_of(url_), *info);
}

Status XrdReader::submit(uint64_t offset, std::span<std::byte> buf, ReadAhead::Done done) {
  auto* handler = new Callback([this, done = std::move(done)](const XrdCl::XRootDStatus& st,
                                                              XrdCl::AnyObject* response) {
    if (!st.IsOK()) return done(XrdEndpoint::xrd_error(st, "read " + url_));
    XrdCl::ChunkInfo* info = nullptr;
    if (response) response->Get(info);
    done(info ? size_t{info->GetLength()} : size_t{0});
  });
  XrdCl::XRootDStatus st = file_->Read(offset, static_cast<uint32_t>(buf.size()), buf.data(), handler);
  if (!st.IsOK()) {
    delete handler;  // XrdCl calls it only for reads it accepted
    return XrdEndpoint::xrd_error(st, "read " + url_);
  }
  return {};
}

// ---- XrdEndpoint ---------------------------------------------------------------------

Result<std::unique_ptr<XrdEndpoint>> XrdEndpoint::create(const std::string& url,
                                                         XrdOptions options) {
  auto parts = parse_endpoint_url(url);
  if (!parts.ok()) return parts.error();
  std::unique_ptr<XrdEndpoint> ep(new XrdEndpoint(url, std::move(parts).value(), options));

  // The checksum the server computes, if any. A server that cannot be
  // reached or refuses the login is no endpoint to work with.
  XrdCl::Buffer arg;
  arg.FromString("chksum");
  XrdCl::Buffer* response = nullptr;
  XrdCl::XRootDStatus st = ep->fs().Query(XrdCl::QueryCode::Config, arg, response);
  std::unique_ptr<XrdCl::Buffer> owned(response);
  if (!st.IsOK() && st.code != XrdCl::errErrorResponse)
    return xrd_error(st, "query the configuration of " + ep->describe());
  if (st.IsOK() && response) {
    // The response lists the configured types as "0:adler32,1:crc32c"; the
    // first one is the default. A server without checksums echoes "chksum".
    std::string value = response->ToString();
    std::string first = value.substr(0, value.find_first_of(",\n "));
    if (auto colon = first.find(':'); colon != std::string::npos) first = first.substr(colon + 1);
    if (auto type = parse_checksum_type(first); type && *type != ChecksumType::None)
      ep->caps_.checksum = *type;
    else if (first != "chksum")
      log::debug(ep->describe(), ": unknown checksum type ", first);
  }
  return ep;
}

XrdEndpoint::XrdEndpoint(const std::string& url, EndpointUrl parts, XrdOptions options)
    : url_(display_url(url)),
      server_(std::move(parts.server)),
      host_(std::move(parts.host)),
      root_(std::move(parts.path)),
      cgi_(std::move(parts.cgi)),
      options_(options) {
  caps_.mtime_resolution = 1000000000;
  caps_.has_owners = false;
  caps_.can_set_owner = false;
  caps_.can_set_mode = true;
  caps_.file_mode_bits = 0777;
  caps_.dir_mode_bits = 0777;
  caps_.can_set_mtime = false;
  caps_.has_symlinks = false;
  caps_.checksum = ChecksumType::None;
}

XrdEndpoint::~XrdEndpoint() = default;

std::string XrdEndpoint::open_url(const std::string& request_path) const {
  std::string url = server_ + "/" + request_path;
  if (options_.shared_connection) return url;
  // XrdCl keys its connections by host and this parameter (URL::GetChannelId,
  // XrdClURL.cc), keeps it for redirections and does not send it to servers.
  return with_cgi(std::move(url), "xrdcl.intent=eosmirror" + std::to_string(thread_number()));
}

XrdCl::FileSystem& XrdEndpoint::fs() {
  int thread = options_.shared_connection ? -1 : thread_number();
  std::lock_guard lock(fs_mutex_);
  std::unique_ptr<XrdCl::FileSystem>& slot = fs_[thread];
  // The URL's parameters go with the login, such as xrd.wantprot.
  if (!slot) slot = std::make_unique<XrdCl::FileSystem>(XrdCl::URL(with_cgi(open_url(""), cgi_)));
  return *slot;
}

Location XrdEndpoint::location() const {
  std::string path = std::filesystem::path(root_).lexically_normal().string();
  while (path.size() > 1 && path.back() == '/') path.pop_back();
  return {host_, path};
}

bool XrdEndpoint::is_temporary(const Entry& entry) const {
  return entry.type == EntryType::File && is_temporary_name(entry.name);
}

std::string XrdEndpoint::absolute(const RelPath& path) const {
  if (path.empty()) return root_;
  return root_ == "/" ? "/" + path : root_ + "/" + path;
}

Result<std::string> XrdEndpoint::request_path(const std::string& abs_path) const {
  auto request = xrootd_request_path(abs_path);
  if (!request.ok()) return request.error();
  return with_cgi(std::move(request).value(), cgi_);
}

Result<Entry> XrdEndpoint::stat(const RelPath& path) {
  auto e = stat_abs(absolute(path));
  if (e.ok() && path.empty()) e.value().name.clear();
  return e;
}

Result<Entry> XrdEndpoint::stat_abs(const std::string& abs) {
  auto request = request_path(abs);
  if (!request.ok()) return request.error();
  XrdCl::StatInfo* info = nullptr;
  XrdCl::XRootDStatus st = fs().Stat(request.value(), info);
  if (!st.IsOK()) return xrd_error(st, "stat " + url_of(abs));
  std::unique_ptr<XrdCl::StatInfo> owned(info);
  return entry_from_stat(name_of(abs), *info);
}

// A server ends a listing with stat information at the first entry it cannot
// stat for another reason than ENOENT, such as a symlink loop, and reports
// success (XrdOssDir::Readdir, XrdOfsDirectory::nextEntry and
// XrdXrootdProtocol::do_DirStat take the error for the end). A listing of
// the names alone stats nothing, so it is taken alongside, and its names
// that the other lacks are stated one by one. Names come one per line,
// unescaped: a name with line breaks can forge lines, so names listed more
// than once are left out.
Result<Listing> XrdEndpoint::list(const RelPath& dir) {
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
  XrdCl::XRootDStatus st = fs().DirList(request.value(), XrdCl::DirListFlags::None, handler);
  if (!st.IsOK()) {
    delete handler;  // XrdCl calls it only for requests it accepted
    return xrd_error(st, "list " + url_of(abs));
  }
  XrdCl::DirectoryList* listing = nullptr;
  st = fs().DirList(request.value(), XrdCl::DirListFlags::Stat, listing);
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
    auto e = stat(join(dir, name));
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
              " whose names are listed more than once");
  result.skipped += forged;
  return result;
}

Status XrdEndpoint::mkdir(const RelPath& path, ModeBits mode) {
  std::string abs = absolute(path);
  auto request = request_path(abs);
  if (!request.ok()) return request.error();
  XrdCl::XRootDStatus st =
      fs().MkDir(request.value(), XrdCl::MkDirFlags::None, access_mode(mode | 0700));
  if (!st.IsOK()) return xrd_error(st, "mkdir " + url_of(abs));
  return {};
}

Status XrdEndpoint::symlink(const RelPath& path, const std::string&) {
  return Error{ErrorKind::Unsupported, "symlinks are not supported on " + url_of(absolute(path))};
}

Status XrdEndpoint::set_metadata(const RelPath& path, const Entry& md, MetaFields fields) {
  std::string abs = absolute(path);
  if (has(fields, MetaFields::Owner))
    return Error{ErrorKind::Unsupported, "owners cannot be set on " + url_of(abs)};
  if (has(fields, MetaFields::Mtime))
    return Error{ErrorKind::Unsupported, "mtimes cannot be set on " + url_of(abs)};
  if (has(fields, MetaFields::Mode) && md.type != EntryType::Symlink) {
    auto request = request_path(abs);
    if (!request.ok()) return request.error();
    XrdCl::XRootDStatus st = fs().ChMod(request.value(), access_mode(md.mode));
    if (!st.IsOK()) return xrd_error(st, "chmod " + url_of(abs));
  }
  return {};
}

Status XrdEndpoint::remove(const RelPath& path, EntryType type) {
  std::string abs = absolute(path);
  if (type == EntryType::Directory) {
    auto request = request_path(abs);
    if (!request.ok()) return request.error();
    XrdCl::XRootDStatus st = fs().RmDir(request.value());
    if (!st.IsOK()) return xrd_error(st, "rmdir " + url_of(abs));
    return {};
  }
  return remove_abs(abs);
}

Status XrdEndpoint::remove_abs(const std::string& abs) {
  auto request = request_path(abs);
  if (!request.ok()) return request.error();
  XrdCl::XRootDStatus st = fs().Rm(request.value());
  if (!st.IsOK()) return xrd_error(st, "remove " + url_of(abs));
  return {};
}

Status XrdEndpoint::rename_abs(const std::string& from, const std::string& to) {
  auto move = [&](const std::string& a, const std::string& b) -> Status {
    auto request_a = request_path(a);
    if (!request_a.ok()) return request_a.error();
    auto request_b = request_path(b);
    if (!request_b.ok()) return request_b.error();
    XrdCl::XRootDStatus st = fs().Mv(request_a.value(), request_b.value());
    if (!st.IsOK()) return xrd_error(st, "rename " + url_of(a) + " to " + url_of(b));
    return {};
  };
  return move_replacing(from, to, move, [&](const std::string& abs) { return remove_abs(abs); });
}

Result<Checksum> XrdEndpoint::query_checksum(const std::string& abs) {
  auto request = request_path(abs);
  if (!request.ok()) return request.error();
  XrdCl::Buffer arg;
  arg.FromString(request.value());
  XrdCl::Buffer* response = nullptr;
  XrdCl::XRootDStatus st = fs().Query(XrdCl::QueryCode::Checksum, arg, response);
  if (!st.IsOK()) return xrd_error(st, "checksum of " + url_of(abs));
  std::unique_ptr<XrdCl::Buffer> owned(response);
  return parse_checksum(response ? response->ToString() : "", url_of(abs));
}

Result<Checksum> XrdEndpoint::stored_checksum(const RelPath& path, const Entry&) {
  if (caps_.checksum == ChecksumType::None) return Checksum{};
  return query_checksum(absolute(path));
}

Result<std::unique_ptr<XrdCl::File>> XrdEndpoint::open_file(const std::string& abs) {
  auto request = request_path(abs);
  if (!request.ok()) return request.error();
  auto file = new_file();
  XrdCl::XRootDStatus st = file->Open(open_url(request.value()), XrdCl::OpenFlags::Read);
  if (!st.IsOK()) return xrd_error(st, "open " + url_of(abs));
  return file;
}

Result<std::unique_ptr<FileReader>> XrdEndpoint::open_read(const RelPath& path) {
  std::string abs = absolute(path);
  auto file = open_file(abs);
  if (!file.ok()) return file.error();
  return std::unique_ptr<FileReader>(
      new XrdReader(std::move(file).value(), url_of(abs), options_.read_window));
}

Result<std::unique_ptr<FileWriter>> XrdEndpoint::open_write(const RelPath& path,
                                                            const CommitSpec& spec) {
  std::string abs = absolute(path);
  std::string temp = parent_of(abs) + "/" + temporary_name(name_of(abs));
  auto request = request_path(temp);
  if (!request.ok()) return request.error();
  auto file = new_write_file();
  ModeBits mode = has(spec.fields, MetaFields::Mode) ? spec.metadata.mode : 0644;
  XrdCl::XRootDStatus st = file->Open(open_url(request.value()),
                                      XrdCl::OpenFlags::New | XrdCl::OpenFlags::Write,
                                      access_mode(mode));
  if (!st.IsOK()) return xrd_error(st, "create " + url_of(temp));
  return std::unique_ptr<FileWriter>(
      new XrdWriter(*this, std::move(file), temp, abs, options_.write_window));
}

}  // namespace eosmirror
