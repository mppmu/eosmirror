// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <atomic>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

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
  // Connects and resolves symlinks in the path, since EOS lists entries
  // under their real paths.
  static Result<std::unique_ptr<EosEndpoint>> create(const std::string& url,
                                                     XrdOptions options = XrdOptions());

  // Whether the server behind the URL is an EOS MGM. An error if the server
  // cannot be asked (unreachable, login refused).
  static Result<bool> is_eos(const std::string& url);

  bool is_temporary(std::string_view name) const override;
  Result<std::string> request_path(const std::string& abs_path) const override;
  Result<Entry> stat(const RelPath& path) override;
  Result<std::vector<Entry>> list(const RelPath& dir) override;
  Status symlink(const RelPath& path, const std::string& target) override;
  Status set_metadata(const RelPath& path, const Entry& metadata, MetaFields fields) override;
  Result<std::unique_ptr<FileReader>> open_read(const RelPath& path) override;
  Result<std::unique_ptr<FileWriter>> open_write(const RelPath& path,
                                                 const CommitSpec& spec) override;
  // The checksum from the listing, without asking.
  Result<Checksum> stored_checksum(const RelPath& path, const Entry& listed) override;

  // Stats an absolute path without following a final symlink; the result is
  // named by the last path component.
  Result<Entry> stat_abs(const std::string& abs_path);

  // Sets the metadata of an absolute path through MGM commands.
  Status set_metadata_abs(const std::string& abs_path, const Entry& metadata, MetaFields fields,
                          bool is_symlink);

  // Runs an MGM fsctl command (mgm.pcmd=<command><args>) on an absolute path,
  // as a single query; it replies "<command>: retc=<errno>".
  Status fsctl(const std::string& abs_path, const std::string& command, const std::string& args);

  struct ProcResult {
    int retc = 0;
    std::string out;
    std::string err;
  };

  // Runs a text command (mgm.cmd=...) or a protobuf command (mgm.cmd.proto=...)
  // on the MGM, as the eos client does.
  Result<ProcResult> proc(const std::string& query);

 private:
  EosEndpoint(const std::string& url, EndpointUrl parts, XrdOptions options);
  Status probe_identity();

  // proc(), telling through answered whether a failure is the server's answer
  // (rather than no answer).
  Result<ProcResult> run_proc(const std::string& query, bool* answered);

  // The path with all symlinks resolved, like realpath(3); a part that does
  // not exist is kept as it is.
  Result<std::string> resolve(const std::string& abs_path);

  // Completes a listing that find cut short with the names of a plain
  // directory listing, each stated on its own.
  Status complete_listing(const std::string& abs_dir, std::vector<Entry>& entries);

  std::atomic<bool> truncation_warned_{false};
};

// A path for a request to EOS: percent-encoded behind "/#curl#", which the
// MGM decodes when the request carries eos.encodepath. The request path adds
// that parameter; further ones follow after '&'.
std::string eos_encoded_path(const std::string& abs_path);
std::string eos_request_path(const std::string& abs_path);

// The RequestProto of the console's "file symlink" command (with force),
// which carries the link path and target as bytes.
std::string symlink_request(const std::string& abs_path, const std::string& target);

// Whether EOS stores a symlink's path and target as they are, and lists
// them back: the path must not contain "#AND#" (which EOS turns into '&'),
// the target must not start like a file or container id ("fid:", "fxid:",
// "cid:", "cxid:") and must not contain a line break.
bool symlink_safe(std::string_view abs_path, std::string_view target);

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

// The names of the subdirectories of abs_dir that a find left out because the
// identity may not read them, from the find's error output. Nothing if the
// output reports anything else, abs_dir itself included.
std::optional<std::vector<std::string>> parse_find_denials(std::string_view err,
                                                           std::string_view abs_dir);

}  // namespace eosmirror
