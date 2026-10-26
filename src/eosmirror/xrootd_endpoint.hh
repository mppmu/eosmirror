// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <memory>
#include <string>

#include "eosmirror/endpoint.hh"
#include "eosmirror/xrd_session.hh"

namespace eosmirror {

// An endpoint on an XRootD server, given as root://host[:port]//path, or
// roots:// for TLS. Opaque parameters of the URL (such as authz tokens) go
// with every request.
//
// Plain XRootD has no symlinks, owners or settable mtimes, so this endpoint
// reports those as missing capabilities. The EOS endpoint adds them through
// MGM commands. Files are written under a temporary name and renamed into
// place on commit, with the checksum verified by a checksum query when the
// server computes one.
class XrdEndpoint : public Endpoint {
 public:
  // client: what sends the requests, by default XrdCl (xrdcl_client()).
  static Result<std::unique_ptr<XrdEndpoint>> create(const std::string& url,
                                                     XrdOptions options = XrdOptions(),
                                                     std::shared_ptr<XrdClient> client = nullptr);
  ~XrdEndpoint() override;

  std::string describe() const override { return session_.url(); }
  // The root, normalized. EOS resolves it on creation.
  Location location() const override { return session_.location(); }
  Capabilities capabilities() const override { return caps_; }
  bool is_temporary(const Entry& entry) const override;

  Result<Entry> stat(const RelPath& path) override;
  Result<Listing> list(const RelPath& dir) override;
  Status mkdir(const RelPath& path, ModeBits mode) override;
  Status symlink(const RelPath& path, const std::string& target) override;
  Status set_metadata(const RelPath& path, const Entry& metadata, MetaFields fields) override;
  Status remove(const RelPath& path, EntryType type) override;
  Result<std::unique_ptr<FileReader>> open_read(const RelPath& path) override;
  Result<std::unique_ptr<FileWriter>> open_write(const RelPath& path,
                                                 const CommitSpec& spec) override;
  // Asks the server, if it computes checksums.
  Result<Checksum> stored_checksum(const RelPath& path, const Entry& listed) override;

  // The absolute path on the server of a relative path, and its URL for
  // messages.
  std::string absolute(const RelPath& path) const { return session_.absolute(path); }
  std::string url_of(const std::string& abs_path) const { return session_.url_of(abs_path); }

  // An absolute path as sent to the server, with the URL's opaque
  // parameters. EOS encodes paths and adds the opaque parameter that says so.
  Result<std::string> request_path(const std::string& abs_path) const {
    return session_.request_path(abs_path);
  }

  // Operations on absolute server paths, used by the writer. A stored
  // checksum of a type eosmirror cannot compute, or none, has type None.
  // The result of a stat is named by the last path component.
  virtual Result<Entry> stat_abs(const std::string& abs_path);
  Result<Checksum> query_checksum(const std::string& abs_path) {
    return session_.query_checksum(abs_path);
  }
  Status remove_abs(const std::string& abs_path) { return session_.remove_abs(abs_path); }
  // Refuses to replace a file by moving it aside where the target may have
  // symlinks (XrdOptions::without_symlinks).
  Status rename_abs(const std::string& from, const std::string& to);
  Status chmod_abs(const std::string& abs_path, ModeBits mode) {
    return session_.chmod_abs(abs_path, mode);
  }

  // As eosmirror::xrd_error().
  static Error xrd_error(const XrdCl::XRootDStatus& status, const std::string& context);

 protected:
  // encoding: how requests write paths.
  XrdEndpoint(const std::string& url, EndpointUrl parts, XrdOptions options, PathEncoding encoding,
              std::shared_ptr<XrdClient> client);

  XrdSession& session() { return session_; }
  const XrdSession& session() const { return session_; }

  Capabilities caps_;

 private:
  XrdSession session_;
};

}  // namespace eosmirror
