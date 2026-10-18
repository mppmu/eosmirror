// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

#include "eosmirror/endpoint.hh"

namespace eosmirror {

struct PosixOptions {
  // Read every written file back and compare its checksum before the rename.
  bool verify_readback = false;
  // Make copies durable without waiting for each: a copy is renamed into
  // place with mtime 0, flush() syncs the file systems that copies went to
  // (syncfs), and the copies renamed before then get their mtimes. Without
  // it, copies get their mtimes before the rename, and nothing is synced.
  bool sync = true;
  // The mode of copied files when modes are not preserved (0666 & ~umask).
  ModeBits default_mode = 0644;
};

class DeferredMtimes;

// An endpoint on a local file system (which includes network file systems
// mounted on the host, such as CephFS or an EOS FUSE mount).
//
// Files are written to ".<name>.eosmirror-<random>" in the target directory
// and renamed into place on commit. Directories are created with the owner's
// rwx bits added, since their exact mode is applied when they are finalized.
class PosixEndpoint : public Endpoint {
 public:
  explicit PosixEndpoint(std::string root, PosixOptions options = PosixOptions());
  // Flushes what is left.
  ~PosixEndpoint() override;

  std::string describe() const override { return root_; }
  Location location() const override { return {"", root_}; }
  Capabilities capabilities() const override;
  bool is_temporary(const Entry& entry) const override;
  bool other_file_system(const Entry& dir) const override;
  // Warns about EOS FUSE mounts.
  std::string target_warning() const override;
  // Probes the mtime resolution, which capabilities() reports as full
  // until then.
  void probe_target() override;

  Result<Entry> stat(const RelPath& path) override;
  Result<Listing> list(const RelPath& dir) override;
  Status mkdir(const RelPath& path, ModeBits mode) override;
  Status symlink(const RelPath& path, const std::string& target) override;
  Status set_metadata(const RelPath& path, const Entry& metadata, MetaFields fields) override;
  Status remove(const RelPath& path, EntryType type) override;
  Result<std::unique_ptr<FileReader>> open_read(const RelPath& path) override;
  Result<std::unique_ptr<FileWriter>> open_write(const RelPath& path,
                                                 const CommitSpec& spec) override;
  // Syncs the file systems of the copies so far and gives them their mtimes.
  Status flush() override;

  // The absolute path of a relative one.
  std::string absolute(const RelPath& path) const;

  // Why the root could not be resolved to its canonical path, if it could
  // not: such an endpoint is not to be used, since the overlap check could
  // not trust the path.
  std::optional<Error> root_error() const { return root_error_; }

 private:
  std::string root_;
  std::optional<Error> root_error_;
  PosixOptions options_;
  std::once_flag probe_once_;
  std::atomic<int32_t> mtime_resolution_{1};
  mutable std::once_flag device_once_;
  mutable std::string device_;  // of the root, as in directory ids
  std::unique_ptr<DeferredMtimes> deferred_;  // with options_.sync
};

// The canonical form of a path that may not exist yet: that of its deepest
// existing ancestor, followed by the rest. Normalizing ".." behind a missing
// component can expose a symlink before it ("missing/../link"), so the
// result is resolved again until it settles.
Result<std::string> canonical_path(const std::string& path);

// The type of the file system that a path is on, from a mount table in the
// format of /proc/self/mountinfo: that of the mount with the longest mount
// point above the path. Nothing if no mount matches.
std::optional<std::string> mount_type(std::string_view mountinfo, std::string_view path);

}  // namespace eosmirror
