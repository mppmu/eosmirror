// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <memory>
#include <string>

#include "eosmirror/endpoint.hh"

namespace XrdCl {
class File;
class FileSystem;
class XRootDStatus;
}  // namespace XrdCl

namespace eosmirror {

struct XrdOptions {
  // Writes of one file kept in flight before write() blocks. Storage nodes
  // handle the requests of a connection one after another, so a deep
  // window only queues.
  int write_window = 2;
};

// The URL without what may carry credentials: a password before the host and
// the opaque parameters (authz tokens and the like).
std::string display_url(std::string_view url);

// The parts of an endpoint URL, proto://[user@]host[:port]//path[?cgi].
struct EndpointUrl {
  std::string server;  // proto://[user@]host:port
  std::string path;    // absolute, without trailing slash
  std::string cgi;     // the opaque parameters, without '?'
};

Result<EndpointUrl> parse_endpoint_url(const std::string& url);

// A request path or URL with opaque parameters added after its own.
std::string with_cgi(std::string request, std::string_view cgi);

// The path of a request to an XRootD server. XRootD has no escaping for
// paths: a '?' starts the opaque parameters, so paths with one are refused.
Result<std::string> xrootd_request_path(const std::string& abs_path);

// An endpoint on an XRootD server, given as root://host[:port]//path, or
// roots:// for TLS. Opaque parameters of the URL (such as authz tokens) go
// with every request.
//
// Plain XRootD has no symlinks, owners or settable mtimes, so this endpoint
// reports those as missing capabilities; the EOS endpoint adds them through
// MGM commands. Files are written under a temporary name and renamed into
// place on commit, with the checksum verified by a checksum query when the
// server computes one.
class XrdEndpoint : public Endpoint {
 public:
  static Result<std::unique_ptr<XrdEndpoint>> create(const std::string& url,
                                                     XrdOptions options = XrdOptions());
  ~XrdEndpoint() override;

  std::string describe() const override { return url_; }
  Capabilities capabilities() const override { return caps_; }
  bool is_temporary(std::string_view name) const override;

  Result<Entry> stat(const RelPath& path) override;
  Result<std::vector<Entry>> list(const RelPath& dir) override;
  Status mkdir(const RelPath& path, ModeBits mode) override;
  Status symlink(const RelPath& path, const std::string& target) override;
  Status set_metadata(const RelPath& path, const Entry& metadata, MetaFields fields) override;
  Status remove(const RelPath& path, EntryType type) override;
  Result<std::unique_ptr<FileReader>> open_read(const RelPath& path) override;
  Result<std::unique_ptr<FileWriter>> open_write(const RelPath& path,
                                                 const CommitSpec& spec) override;

  // The absolute path on the server of a relative path, and its URL for
  // messages.
  std::string absolute(const RelPath& path) const;
  std::string url_of(const std::string& abs_path) const {
    return display_url(server_) + "/" + abs_path;
  }

  // An absolute path as sent to the server, with the URL's opaque
  // parameters. EOS encodes paths and adds the opaque parameter that says so.
  virtual Result<std::string> request_path(const std::string& abs_path) const;

  // Operations on absolute server paths, used by the writer. A stored
  // checksum of a type eosmirror cannot compute, or none, has type None.
  Result<Checksum> query_checksum(const std::string& abs_path);
  Status remove_abs(const std::string& abs_path);
  Status rename_abs(const std::string& from, const std::string& to);

  // Converts an XrdCl status into an error with the given context.
  static Error xrd_error(const XrdCl::XRootDStatus& status, const std::string& context);

  // A file object for an upload, with write recovery turned off.
  static std::unique_ptr<XrdCl::File> new_write_file();

 protected:
  XrdEndpoint(const std::string& url, EndpointUrl parts, XrdOptions options);

  // The URL for File::Open of a request path.
  std::string open_url(const std::string& request_path) const {
    return server_ + "/" + request_path;
  }

  std::string url_;     // as given, for display (display_url)
  std::string server_;  // root://[user[:password]@]host:port
  std::string root_;    // the path part, without trailing slash
  std::string cgi_;     // the URL's opaque parameters
  XrdOptions options_;
  Capabilities caps_;
  std::unique_ptr<XrdCl::FileSystem> fs_;
};

}  // namespace eosmirror
