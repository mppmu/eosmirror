// SPDX-License-Identifier: GPL-3.0-or-later
#include "eosmirror/xrootd_endpoint.hh"

#include <XrdCl/XrdClFile.hh>
#include <XrdCl/XrdClFileSystem.hh>
#include <XrdCl/XrdClStatus.hh>
#include <XrdCl/XrdClURL.hh>
#include <XrdCl/XrdClXRootDResponses.hh>
#include <XProtocol/XProtocol.hh>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <optional>
#include <sstream>
#include <vector>

#include "eosmirror/log.hh"

namespace eosmirror {

namespace {

constexpr std::string_view kTempMarker = ".eosmirror-";

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
  e.mtime = {static_cast<int64_t>(info.GetModTime()), 0};
  if (info.ExtendedFormat()) {
    // The raw octal string from the server ("0640"); GetModeAsOctString()
    // would render it symbolically despite its name.
    std::string oct = info.GetModeAsString();
    e.mode = static_cast<ModeBits>(std::strtoul(oct.c_str(), nullptr, 8)) & 07777;
  }
  return e;
}

Result<Checksum> parse_checksum(const std::string& text, const std::string& context) {
  std::istringstream in(text);
  std::string type, value;
  in >> type >> value;
  auto parsed = parse_checksum_type(type);
  if (!parsed || value.empty())
    return Error{ErrorKind::Other, "unexpected checksum response for " + context + ": " + text};
  for (auto& c : value) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return Checksum{*parsed, value};
}

class XrdReader : public FileReader {
 public:
  XrdReader(std::unique_ptr<XrdCl::File> file, std::string url)
      : file_(std::move(file)), url_(std::move(url)) {}

  ~XrdReader() override {
    XrdCl::XRootDStatus st = file_->Close();
    (void)st;
  }

  Result<size_t> read(uint64_t offset, std::span<std::byte> buf) override {
    size_t done = 0;
    while (done < buf.size()) {
      uint32_t got = 0;
      XrdCl::XRootDStatus st = file_->Read(
          offset + done, static_cast<uint32_t>(buf.size() - done), buf.data() + done, got);
      if (!st.IsOK()) return XrdEndpoint::xrd_error(st, "read " + url_);
      if (got == 0) break;
      done += got;
    }
    return done;
  }

  Result<Entry> stat() override {
    XrdCl::StatInfo* info = nullptr;
    XrdCl::XRootDStatus st = file_->Stat(/*force=*/true, info);
    if (!st.IsOK()) return XrdEndpoint::xrd_error(st, "stat " + url_);
    std::unique_ptr<XrdCl::StatInfo> owned(info);
    return entry_from_stat(name_of(url_), *info);
  }

 private:
  std::unique_ptr<XrdCl::File> file_;
  std::string url_;
};

// Writes are issued asynchronously with up to `window` in flight, each from
// its own copy of the data; commit() waits for all of them, closes, verifies
// the stored checksum and renames the file into place.
class XrdWriter : public FileWriter {
 public:
  XrdWriter(XrdEndpoint& ep, std::unique_ptr<XrdCl::File> file, std::string temp_abs,
            std::string final_abs, int window)
      : ep_(ep),
        file_(std::move(file)),
        temp_(std::move(temp_abs)),
        final_(std::move(final_abs)),
        buffers_(static_cast<size_t>(std::max(window, 1))) {
    for (size_t i = 0; i < buffers_.size(); ++i) free_.push_back(i);
  }

  ~XrdWriter() override { abort(); }

  Status write(uint64_t offset, std::span<const std::byte> data) override {
    if (offset != written_) return Error{ErrorKind::Other, "non-sequential write to " + temp_};
    size_t index;
    {
      std::unique_lock lock(mutex_);
      cv_.wait(lock, [&] { return !free_.empty(); });
      if (error_) return *error_;
      index = free_.back();
      free_.pop_back();
      ++in_flight_;
    }
    buffers_[index].assign(data.begin(), data.end());
    XrdCl::XRootDStatus st = file_->Write(offset, static_cast<uint32_t>(data.size()),
                                          buffers_[index].data(), new Pending{this, index});
    if (!st.IsOK()) {
      std::lock_guard lock(mutex_);
      free_.push_back(index);
      --in_flight_;
      return XrdEndpoint::xrd_error(st, "write " + temp_);
    }
    written_ += data.size();
    return {};
  }

  Result<CommitInfo> commit(const CommitSpec& spec) override {
    Status s = finish(spec);
    if (!s.ok()) {
      abort();
      return s.error();
    }
    return CommitInfo{verified_};
  }

  void abort() override {
    if (file_) {
      drain();
      XrdCl::XRootDStatus st = file_->Close();
      (void)st;
      file_.reset();
    }
    if (!temp_exists_) return;
    temp_exists_ = false;
    Status removed = ep_.remove_abs(temp_);
    if (!removed.ok() && removed.error().kind != ErrorKind::NotFound)
      log::warn("cannot remove temporary ", temp_, ": ", removed.error().describe());
  }

 private:
  struct Pending : XrdCl::ResponseHandler {
    XrdWriter* writer;
    size_t index;
    Pending(XrdWriter* w, size_t i) : writer(w), index(i) {}
    void HandleResponse(XrdCl::XRootDStatus* status, XrdCl::AnyObject* response) override {
      writer->completed(index, *status);
      delete status;
      delete response;
      delete this;
    }
  };

  void completed(size_t index, const XrdCl::XRootDStatus& st) {
    std::lock_guard lock(mutex_);
    if (!st.IsOK() && !error_) error_ = stalled(XrdEndpoint::xrd_error(st, "write " + temp_));
    if (st.IsOK()) last_ack_ = std::chrono::steady_clock::now();
    free_.push_back(index);
    --in_flight_;
    cv_.notify_all();
  }

  // Adds how long ago the server last acknowledged a write, which tells a
  // stalled connection from an immediate refusal.
  Error stalled(Error e) const {
    auto secs = std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::steady_clock::now() - last_ack_)
                    .count();
    e.message += " (last write acknowledged " + std::to_string(secs) + " s earlier)";
    return e;
  }

  void drain() {
    std::unique_lock lock(mutex_);
    cv_.wait(lock, [&] { return in_flight_ == 0; });
  }

  Status finish(const CommitSpec& spec) {
    drain();
    {
      std::lock_guard lock(mutex_);
      if (error_) return *error_;
    }
    if (written_ != spec.size)
      return Error{ErrorKind::Changed, "wrote " + std::to_string(written_) + " bytes to " + temp_ +
                                           ", expected " + std::to_string(spec.size)};
    XrdCl::XRootDStatus st = file_->Close();
    file_.reset();
    if (!st.IsOK()) return stalled(XrdEndpoint::xrd_error(st, "close " + temp_));
    if (spec.checksum.type != ChecksumType::None &&
        spec.checksum.type == ep_.capabilities().checksum) {
      auto stored = ep_.query_checksum(temp_);
      if (!stored.ok()) return stored.error();
      if (!(stored.value() == spec.checksum))
        return Error{ErrorKind::Checksum, temp_ + " has checksum " + stored.value().hex +
                                              " instead of " + spec.checksum.hex};
      verified_ = true;
    } else if (spec.require_verification) {
      return Error{ErrorKind::Unsupported, "the server computes no checksum to verify " + temp_ +
                                               " against"};
    }
    Status renamed = ep_.rename_abs(temp_, final_);
    if (renamed.ok()) temp_exists_ = false;
    return renamed;
  }

  XrdEndpoint& ep_;
  std::unique_ptr<XrdCl::File> file_;
  std::string temp_;
  std::string final_;
  std::vector<std::vector<std::byte>> buffers_;
  std::vector<size_t> free_;
  std::mutex mutex_;
  std::condition_variable cv_;
  int in_flight_ = 0;
  std::optional<Error> error_;
  uint64_t written_ = 0;
  bool temp_exists_ = true;
  bool verified_ = false;
  std::chrono::steady_clock::time_point last_ack_ = std::chrono::steady_clock::now();
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
    if (err >= kXR_ArgInvalid) err = XProtocol::toErrno(err);
    Error e = errno_error(err, context);
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
  auto file = std::make_unique<XrdCl::File>();
  file->SetProperty("WriteRecovery", "false");
  return file;
}

Result<std::unique_ptr<XrdEndpoint>> XrdEndpoint::create(const std::string& url,
                                                         XrdOptions options) {
  XrdCl::URL parsed(url);
  if (!parsed.IsValid() || parsed.GetProtocol().empty() || parsed.GetHostName().empty())
    return Error{ErrorKind::Other, "invalid XRootD URL: " + display_url(url)};
  std::string server = parsed.GetProtocol() + "://" + parsed.GetHostId();
  std::string root = parsed.GetPath();
  while (root.size() > 1 && root.back() == '/') root.pop_back();
  if (root.empty() || root[0] != '/')
    return Error{ErrorKind::Other, "XRootD URL needs an absolute path: " + display_url(url)};
  std::unique_ptr<XrdEndpoint> ep(new XrdEndpoint(url, server, root, options));

  // The checksum the server computes, if any.
  XrdCl::Buffer arg;
  arg.FromString("chksum");
  XrdCl::Buffer* response = nullptr;
  XrdCl::XRootDStatus st = ep->fs_->Query(XrdCl::QueryCode::Config, arg, response);
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
  delete response;
  return ep;
}

XrdEndpoint::XrdEndpoint(std::string url, std::string server, std::string root,
                         XrdOptions options)
    : url_(display_url(url)),
      server_(std::move(server)),
      root_(std::move(root)),
      options_(options),
      fs_(std::make_unique<XrdCl::FileSystem>(XrdCl::URL(server_))) {
  caps_.mtime_resolution = 1000000000;
  caps_.can_set_owner = false;
  caps_.can_set_mode = true;
  caps_.mode_bits = 0777;
  caps_.can_set_mtime = false;
  caps_.has_symlinks = false;
  caps_.checksum = ChecksumType::None;
}

XrdEndpoint::~XrdEndpoint() = default;

bool XrdEndpoint::is_temporary(std::string_view name) const {
  return name.size() > 1 && name[0] == '.' && name.find(kTempMarker) != std::string_view::npos;
}

std::string XrdEndpoint::absolute(const RelPath& path) const {
  if (path.empty()) return root_;
  return root_ == "/" ? "/" + path : root_ + "/" + path;
}

Result<std::string> XrdEndpoint::request_path(const std::string& abs_path) const {
  return xrootd_request_path(abs_path);
}

Result<Entry> XrdEndpoint::stat(const RelPath& path) {
  std::string abs = absolute(path);
  auto request = request_path(abs);
  if (!request.ok()) return request.error();
  XrdCl::StatInfo* info = nullptr;
  XrdCl::XRootDStatus st = fs_->Stat(request.value(), info);
  if (!st.IsOK()) return xrd_error(st, "stat " + url_of(abs));
  std::unique_ptr<XrdCl::StatInfo> owned(info);
  return entry_from_stat(path.empty() ? "" : name_of(path), *info);
}

Result<std::vector<Entry>> XrdEndpoint::list(const RelPath& dir) {
  std::string abs = absolute(dir);
  auto request = request_path(abs);
  if (!request.ok()) return request.error();
  XrdCl::DirectoryList* listing = nullptr;
  XrdCl::XRootDStatus st = fs_->DirList(request.value(), XrdCl::DirListFlags::Stat, listing);
  if (!st.IsOK()) return xrd_error(st, "list " + url_of(abs));
  std::unique_ptr<XrdCl::DirectoryList> owned(listing);
  std::vector<Entry> entries;
  entries.reserve(listing->GetSize());
  for (auto it = listing->Begin(); it != listing->End(); ++it) {
    XrdCl::DirectoryList::ListEntry* item = *it;
    if (!item->GetStatInfo()) {
      // Servers without stat support in listings: one stat per entry.
      auto e = stat(join(dir, item->GetName()));
      if (!e.ok()) {
        if (e.error().kind == ErrorKind::NotFound) continue;
        if (e.error().kind == ErrorKind::Unsupported) {
          log::warn("skipping ", e.error().describe());
          continue;
        }
        return e.error();
      }
      entries.push_back(std::move(e).value());
      continue;
    }
    entries.push_back(entry_from_stat(item->GetName(), *item->GetStatInfo()));
  }
  return entries;
}

Status XrdEndpoint::mkdir(const RelPath& path, ModeBits mode) {
  std::string abs = absolute(path);
  auto request = request_path(abs);
  if (!request.ok()) return request.error();
  XrdCl::XRootDStatus st =
      fs_->MkDir(request.value(), XrdCl::MkDirFlags::None, access_mode(mode | 0700));
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
    XrdCl::XRootDStatus st = fs_->ChMod(request.value(), access_mode(md.mode));
    if (!st.IsOK()) return xrd_error(st, "chmod " + url_of(abs));
  }
  return {};
}

Status XrdEndpoint::remove(const RelPath& path, EntryType type) {
  std::string abs = absolute(path);
  if (type == EntryType::Directory) {
    auto request = request_path(abs);
    if (!request.ok()) return request.error();
    XrdCl::XRootDStatus st = fs_->RmDir(request.value());
    if (!st.IsOK()) return xrd_error(st, "rmdir " + url_of(abs));
    return {};
  }
  return remove_abs(abs);
}

Status XrdEndpoint::remove_abs(const std::string& abs) {
  auto request = request_path(abs);
  if (!request.ok()) return request.error();
  XrdCl::XRootDStatus st = fs_->Rm(request.value());
  if (!st.IsOK()) return xrd_error(st, "remove " + url_of(abs));
  return {};
}

Status XrdEndpoint::rename_abs(const std::string& from, const std::string& to) {
  auto request_from = request_path(from);
  if (!request_from.ok()) return request_from.error();
  auto request_to = request_path(to);
  if (!request_to.ok()) return request_to.error();
  XrdCl::XRootDStatus st = fs_->Mv(request_from.value(), request_to.value());
  if (st.IsOK()) return {};
  Error e = xrd_error(st, "rename " + url_of(from) + " to " + url_of(to));
  if (e.kind != ErrorKind::Exists) return e;
  // The server refuses to replace an existing file: remove it first. This
  // leaves a short window without the file, which EOS avoids with atomic
  // uploads.
  Status removed = remove_abs(to);
  if (!removed.ok()) return removed;
  st = fs_->Mv(request_from.value(), request_to.value());
  if (!st.IsOK()) return xrd_error(st, "rename " + url_of(from) + " to " + url_of(to));
  return {};
}

Result<Checksum> XrdEndpoint::query_checksum(const std::string& abs) {
  auto request = request_path(abs);
  if (!request.ok()) return request.error();
  XrdCl::Buffer arg;
  arg.FromString(request.value());
  XrdCl::Buffer* response = nullptr;
  XrdCl::XRootDStatus st = fs_->Query(XrdCl::QueryCode::Checksum, arg, response);
  if (!st.IsOK()) return xrd_error(st, "checksum of " + url_of(abs));
  std::unique_ptr<XrdCl::Buffer> owned(response);
  return parse_checksum(response ? response->ToString() : "", url_of(abs));
}

Result<std::unique_ptr<FileReader>> XrdEndpoint::open_read(const RelPath& path) {
  std::string abs = absolute(path);
  auto request = request_path(abs);
  if (!request.ok()) return request.error();
  auto file = std::make_unique<XrdCl::File>();
  XrdCl::XRootDStatus st = file->Open(open_url(request.value()), XrdCl::OpenFlags::Read);
  if (!st.IsOK()) return xrd_error(st, "open " + url_of(abs));
  return std::unique_ptr<FileReader>(new XrdReader(std::move(file), url_of(abs)));
}

Result<std::unique_ptr<FileWriter>> XrdEndpoint::open_write(const RelPath& path,
                                                            const CommitSpec& spec) {
  std::string abs = absolute(path);
  std::string name = name_of(abs);
  if (name.size() > 231) name.resize(231);  // keep the temporary name within NAME_MAX
  std::string temp = parent_of(abs) + "/." + name + std::string(kTempMarker) + random_suffix();
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
