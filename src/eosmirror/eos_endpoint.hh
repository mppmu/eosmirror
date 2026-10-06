// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

#include "eosmirror/xrootd_endpoint.hh"

namespace eosmirror {

// An endpoint on an EOS instance, given as root://mgm[:port]//path.
//
// On top of XRootD, EOS has symlinks, owners, nanosecond mtimes and
// listings with full metadata, all reached through the MGM's commands the
// way the eos client sends them (see docs/design.md). Files are uploaded
// atomically: EOS renames them into place at close.
class EosEndpoint : public XrdEndpoint {
 public:
  static Result<std::unique_ptr<EosEndpoint>> create(const std::string& url,
                                                     XrdOptions options = XrdOptions());

  // Whether the server behind the URL is an EOS MGM.
  static bool is_eos(const std::string& url);

  bool is_temporary(std::string_view name) const override;
  Result<std::string> request_path(const std::string& abs_path) const override;
  Result<Entry> stat(const RelPath& path) override;
  Result<std::vector<Entry>> list(const RelPath& dir) override;
  Status symlink(const RelPath& path, const std::string& target) override;
  Status set_metadata(const RelPath& path, const Entry& metadata, MetaFields fields) override;
  Result<std::unique_ptr<FileReader>> open_read(const RelPath& path) override;
  Result<std::unique_ptr<FileWriter>> open_write(const RelPath& path,
                                                 const CommitSpec& spec) override;

  // The checksum type a directory forces on its files (None if none is set).
  Result<ChecksumType> directory_checksum(const std::string& abs_dir);

  // Sets the metadata of an absolute path through MGM commands.
  Status set_metadata_abs(const std::string& abs_path, const Entry& metadata, MetaFields fields,
                          bool is_symlink);

  struct ProcResult {
    int retc = 0;
    std::string out;
    std::string err;
  };

  // Runs a text command (mgm.cmd=...) or a protobuf command (mgm.cmd.proto=...)
  // on the MGM, as the eos client does.
  Result<ProcResult> proc(const std::string& query);

 private:
  EosEndpoint(std::string url, std::string server, std::string root, XrdOptions options);
  Status probe_identity();

  std::mutex cache_mutex_;
  std::unordered_map<std::string, ChecksumType> directory_checksums_;
};

// A path for a request to EOS: percent-encoded behind "/#curl#", which the
// MGM decodes when the request carries eos.encodepath. The request path adds
// that parameter; further ones follow after '&'.
std::string eos_encoded_path(const std::string& abs_path);
std::string eos_request_path(const std::string& abs_path);

// Whether a value can go unencoded into the opaque part of a request: no
// '&', '=', '?', '#', '%' or control characters.
bool opaque_safe(std::string_view value);

// Splits the reply to an MGM command, "mgm.proc.stdout=...&mgm.proc.stderr=...
// &mgm.proc.retc=N". The output can contain anything, file names included,
// so the markers are searched from the end. A reply of another form is
// returned whole as output.
EosEndpoint::ProcResult parse_proc_reply(const std::string& reply);

// One line of the find listing of the directory abs_dir (see docs/design.md)
// as an entry named relative to abs_dir; nothing for abs_dir itself. A line
// that cannot be parsed unambiguously or names no valid entry of abs_dir is
// an error.
Result<std::optional<Entry>> parse_find_line(std::string_view line, std::string_view abs_dir);

}  // namespace eosmirror
