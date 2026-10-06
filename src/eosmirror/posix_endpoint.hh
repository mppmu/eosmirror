// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

#include "eosmirror/endpoint.hh"

namespace eosmirror {

// An endpoint on a local file system (which includes network file systems
// mounted on the host, such as CephFS or an EOS FUSE mount).
//
// Files are written to ".<name>.eosmirror-<random>" in the target directory
// and renamed into place on commit. Directories are created with the owner's
// rwx bits added, since their exact mode is applied when they are finalized.
struct PosixOptions {
  // Read every written file back and compare its checksum before the rename.
  bool verify_readback = false;
  // fsync files before the rename.
  bool fsync = false;
  // The mode of copied files when modes are not preserved (0666 & ~umask).
  ModeBits default_mode = 0644;
};

class PosixEndpoint : public Endpoint {
 public:
  explicit PosixEndpoint(std::string root, PosixOptions options = PosixOptions());

  std::string describe() const override { return root_; }
  Capabilities capabilities() const override;
  bool is_temporary(std::string_view name) const override;
  // Warns about EOS FUSE mounts.
  std::string target_warning() const override;
  // Probes the mtime resolution, which capabilities() reports as full
  // until then.
  void probe_target() override;

  Result<Entry> stat(const RelPath& path) override;
  Result<std::vector<Entry>> list(const RelPath& dir) override;
  Status mkdir(const RelPath& path, ModeBits mode) override;
  Status symlink(const RelPath& path, const std::string& target) override;
  Status set_metadata(const RelPath& path, const Entry& metadata, MetaFields fields) override;
  Status remove(const RelPath& path, EntryType type) override;
  Result<std::unique_ptr<FileReader>> open_read(const RelPath& path) override;
  Result<std::unique_ptr<FileWriter>> open_write(const RelPath& path,
                                                 const CommitSpec& spec) override;

  // The absolute path of a relative one.
  std::string absolute(const RelPath& path) const;

 private:
  std::string root_;
  PosixOptions options_;
  std::once_flag probe_once_;
  std::atomic<int32_t> mtime_resolution_{1};
};

// The type of the file system that a path is on, from a mount table in the
// format of /proc/self/mountinfo: that of the mount with the longest mount
// point above the path. Nothing if no mount matches.
std::optional<std::string> mount_type(std::string_view mountinfo, std::string_view path);

}  // namespace eosmirror
