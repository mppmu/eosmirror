// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <memory>
#include <string>

#include "eosmirror/endpoint.hh"

namespace XrdCl {
class FileSystem;
class XRootDStatus;
}  // namespace XrdCl

namespace eosmirror {

// Applies the process-wide XrdCl settings the endpoints rely on.
void configure_xrdcl();

struct XrdOptions {
  // Writes of one file kept in flight before write() blocks. Storage nodes
  // handle the requests of a connection one after another, so a deep
  // window only queues.
  int write_window = 2;
};

// An endpoint on an XRootD server, given as root://host[:port]//path.
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

  // The absolute path on the server, and the full URL, of a relative path.
  std::string absolute(const RelPath& path) const;
  std::string url_of(const std::string& abs_path) const { return server_ + "/" + abs_path; }

  // Operations on absolute server paths, used by the writer.
  Result<Checksum> query_checksum(const std::string& abs_path);
  Status remove_abs(const std::string& abs_path);
  Status rename_abs(const std::string& from, const std::string& to);

  // Converts an XrdCl status into an error with the given context.
  static Error xrd_error(const XrdCl::XRootDStatus& status, const std::string& context);

 protected:
  XrdEndpoint(std::string url, std::string server, std::string root, XrdOptions options);

  std::string url_;     // as given
  std::string server_;  // root://host:port
  std::string root_;    // the path part, without trailing slash
  XrdOptions options_;
  Capabilities caps_;
  std::unique_ptr<XrdCl::FileSystem> fs_;
};

}  // namespace eosmirror
