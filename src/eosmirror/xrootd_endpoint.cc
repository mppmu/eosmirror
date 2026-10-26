// SPDX-License-Identifier: GPL-3.0-or-later
#include "eosmirror/xrootd_endpoint.hh"

#include <XrdCl/XrdClFile.hh>
#include <XrdCl/XrdClFileSystem.hh>
#include <XrdCl/XrdClXRootDResponses.hh>

#include <memory>
#include <span>
#include <string>

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

// Reads a file as XrdReadFile does.
class XrdReader final : public FileReader {
 public:
  XrdReader(std::unique_ptr<XrdFileApi> file, std::string url, int window)
      : file_(std::move(file), std::move(url), window) {}

  Result<size_t> read(uint64_t offset, std::span<std::byte> buf) override {
    return file_.read(offset, buf);
  }
  Result<Chunk> read_chunk(uint64_t offset, uint64_t end, BufferPool& pool) override {
    return file_.read_chunk(offset, end, pool);
  }
  Result<Entry> stat() override { return file_.stat(true); }

 private:
  XrdReadFile file_;
};

// Writes to a temporary name; commit() waits for the writes, closes,
// verifies the stored checksum and renames the file into place.
class XrdWriter final : public FileWriter {
 public:
  XrdWriter(XrdEndpoint& ep, std::unique_ptr<XrdFileApi> file, std::string temp_abs,
            std::string final_abs, int window)
      : ep_(ep),
        temp_(temp_abs),
        final_(std::move(final_abs)),
        file_(std::move(file), std::move(temp_abs), window) {}

  ~XrdWriter() override { (void)abort(); }

  Status write(Chunk chunk) override { return file_.write(std::move(chunk)); }

  Result<CommitInfo> commit(const CommitSpec& spec) override {
    Status s = finish(spec);
    if (!s.ok()) {
      (void)abort();
      return s.error();
    }
    return CommitInfo{verified_};
  }

  Status abort() override {
    file_.close_quietly();
    if (!temp_exists_) return {};
    temp_exists_ = false;
    Status removed = ep_.remove_abs(temp_);
    if (!removed.ok() && removed.error().kind != ErrorKind::NotFound)
      log::warn("cannot remove temporary ", temp_, ": ", removed.error().describe());
    return {};  // a temporary left behind is not in place
  }

 private:
  Status finish(const CommitSpec& spec) {
    if (Status s = file_.complete(spec.size); !s.ok()) return s;
    if (Status s = file_.close(); !s.ok()) return s;
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
    // The server gives a new file the mode of the open plus the owner's read
    // and write bits (XrdXrootdXeq.cc, do_Open).
    if (has(spec.fields, MetaFields::Mode) && (spec.metadata.mode & 0600) != 0600) {
      if (Status s = ep_.chmod_abs(temp_, spec.metadata.mode); !s.ok()) return s;
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
  XrdWriteFile file_;
};

}  // namespace

// ---- XrdEndpoint ---------------------------------------------------------------------

Result<std::unique_ptr<XrdEndpoint>> XrdEndpoint::create(const std::string& url, XrdOptions options,
                                                         std::shared_ptr<XrdClient> client) {
  auto parts = parse_endpoint_url(url);
  if (!parts.ok()) return parts.error();
  std::unique_ptr<XrdEndpoint> ep(
      new XrdEndpoint(url, std::move(parts).value(), options, std::move(client)));

  // The checksum the server computes, if any. A server that cannot be
  // reached or refuses the login is no endpoint to work with.
  XrdCl::Buffer arg;
  arg.FromString("chksum");
  XrdCl::Buffer* response = nullptr;
  XrdCl::XRootDStatus st = ep->session_.fs().query(XrdCl::QueryCode::Config, arg, response);
  std::unique_ptr<XrdCl::Buffer> owned(response);
  if (!st.IsOK() && st.code != XrdCl::errErrorResponse)
    return xrd_error(st, "query the configuration of " + ep->describe());
  if (st.IsOK() && response) {
    // The response lists the configured types as "0:adler32,1:crc32c". The
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

XrdEndpoint::XrdEndpoint(const std::string& url, EndpointUrl parts, XrdOptions options,
                         std::shared_ptr<XrdClient> client)
    : session_(url, std::move(parts), options, PathEncoding::XRootD, std::move(client)) {
  caps_.mtime_resolution = 1000000000;
  caps_.has_owners = false;
  caps_.can_set_owner = false;
  caps_.can_set_mode = true;
  caps_.file_mode_bits = 0777;
  caps_.dir_mode_bits = 0777;
  caps_.can_set_mtime = false;
  caps_.has_symlinks = false;
  caps_.follows_symlinks = true;
  caps_.checksum = ChecksumType::None;
}

XrdEndpoint::~XrdEndpoint() = default;

Error XrdEndpoint::xrd_error(const XrdCl::XRootDStatus& status, const std::string& context) {
  return eosmirror::xrd_error(status, context);
}

bool XrdEndpoint::is_temporary(const Entry& entry) const {
  return entry.type == EntryType::File && is_temporary_name(entry.name);
}

Result<Entry> XrdEndpoint::stat(const RelPath& path) {
  auto e = stat_abs(absolute(path));
  if (e.ok() && path.empty()) e.value().name.clear();
  return e;
}

Result<Listing> XrdEndpoint::list(const RelPath& dir) {
  return session_.list(dir);
}

Status XrdEndpoint::mkdir(const RelPath& path, ModeBits mode) {
  return session_.mkdir_abs(absolute(path), mode);
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
  if (has(fields, MetaFields::Mode) && md.type != EntryType::Symlink)
    return chmod_abs(abs, md.mode);
  return {};
}

Status XrdEndpoint::remove(const RelPath& path, EntryType type) {
  std::string abs = absolute(path);
  if (type == EntryType::Directory) return session_.rmdir_abs(abs);
  return remove_abs(abs);
}

Status XrdEndpoint::rename_abs(const std::string& from, const std::string& to) {
  if (caps_.follows_symlinks && !session_.options().without_symlinks) {
    Status moved = session_.move_abs(from, to);
    if (moved.ok() || moved.error().kind != ErrorKind::Exists) return moved;
    return Error{ErrorKind::Unsupported,
                 "the server renames nothing over " + session_.url_of(to) +
                     ", which it refuses for symlinks, and removing a symlink deletes what it "
                     "points to: replacing it needs --target-without-symlinks, which states "
                     "that the target has none"};
  }
  return session_.rename_abs(from, to);
}

Result<Checksum> XrdEndpoint::stored_checksum(const RelPath& path, const Entry&) {
  if (caps_.checksum == ChecksumType::None) return Checksum{};
  return query_checksum(absolute(path));
}

Result<std::unique_ptr<FileReader>> XrdEndpoint::open_read(const RelPath& path) {
  std::string abs = absolute(path);
  auto file = session_.open_file(abs);
  if (!file.ok()) return file.error();
  return std::unique_ptr<FileReader>(
      new XrdReader(std::move(file).value(), url_of(abs), session_.options().read_window));
}

Result<std::unique_ptr<FileWriter>> XrdEndpoint::open_write(const RelPath& path,
                                                            const CommitSpec& spec) {
  std::string abs = absolute(path);
  std::string temp = parent_of(abs) + "/" + temporary_name(name_of(abs));
  auto request = request_path(temp);
  if (!request.ok()) return request.error();
  auto file = session_.new_write_file();
  ModeBits mode = has(spec.fields, MetaFields::Mode) ? spec.metadata.mode : 0644;
  XrdCl::XRootDStatus st =
      file->open(session_.open_url(request.value()),
                 XrdCl::OpenFlags::New | XrdCl::OpenFlags::Write, access_mode(mode));
  if (!st.IsOK()) return xrd_error(st, "create " + url_of(temp));
  return std::unique_ptr<FileWriter>(
      new XrdWriter(*this, std::move(file), temp, abs, session_.options().write_window));
}

}  // namespace eosmirror
