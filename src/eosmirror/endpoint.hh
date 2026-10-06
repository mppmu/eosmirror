// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "eosmirror/checksum.hh"
#include "eosmirror/error.hh"
#include "eosmirror/types.hh"

namespace eosmirror {

// What an endpoint can do, so that the engine can adapt to it.
struct Capabilities {
  // The resolution of stored mtimes in nanoseconds (1 = full, 1000000000 = seconds).
  int32_t mtime_resolution = 1;
  // Whether owner and group can be set to arbitrary ids.
  bool can_set_owner = false;
  // Whether modes can be set.
  bool can_set_mode = true;
  // Whether mtimes can be set. Without it, files are compared by size only.
  bool can_set_mtime = true;
  // Whether symlinks exist on the endpoint.
  bool has_symlinks = true;
  // Whether symlinks have owners of their own that can be set.
  bool symlink_owner = true;
  // The checksum the endpoint computes itself for stored files (None if it
  // computes none), which a copy verifies against.
  ChecksumType checksum = ChecksumType::None;
};

// Sequential reader of one file.
class FileReader {
 public:
  virtual ~FileReader() = default;

  // Reads up to buf.size() bytes at offset; a short count means end of file.
  virtual Result<size_t> read(uint64_t offset, std::span<std::byte> buf) = 0;

  // The file's current size and mtime, to detect changes during the copy.
  virtual Result<Entry> stat() = 0;
};

// What a committed file must look like.
struct CommitSpec {
  uint64_t size = 0;
  Entry metadata;  // owner, mode and mtime to apply
  MetaFields fields = MetaFields::All;
  Checksum checksum;  // of the written data; None if not computed
  // Refuse to commit unless the stored data was verified against the checksum.
  bool require_verification = false;
};

struct CommitInfo {
  bool verified = false;  // the stored data was compared with the checksum
};

// Writer of one file to a temporary location. Nothing is visible under the
// final name until commit() succeeds.
class FileWriter {
 public:
  virtual ~FileWriter() = default;

  // Writes data at offset; offsets must be sequential and contiguous.
  virtual Status write(uint64_t offset, std::span<const std::byte> data) = 0;

  // Finishes the file: applies the metadata, verifies size and checksum
  // against what the endpoint stored, and renames it into place.
  virtual Result<CommitInfo> commit(const CommitSpec& spec) = 0;

  // Discards the file. Called instead of commit(), or after a failed one.
  virtual void abort() = 0;
};

class Endpoint {
 public:
  virtual ~Endpoint() = default;

  // The endpoint for messages, e.g. the URL or path it was created from.
  virtual std::string describe() const = 0;
  virtual Capabilities capabilities() const = 0;

  // Whether an entry name is one of the endpoint's own temporary files, so
  // that listings of a target never report them as foreign.
  virtual bool is_temporary(std::string_view /*name*/) const { return false; }

  // Stats an entry without following symlinks. The result's name is the
  // last path component.
  virtual Result<Entry> stat(const RelPath& path) = 0;

  // Lists a directory with full metadata per entry, in no particular order.
  virtual Result<std::vector<Entry>> list(const RelPath& dir) = 0;

  virtual Status mkdir(const RelPath& path, ModeBits mode) = 0;

  // Creates or replaces a symlink.
  virtual Status symlink(const RelPath& path, const std::string& target) = 0;

  // Applies the selected fields of the metadata to the entry itself (not to
  // a symlink's target). Modes of symlinks are ignored.
  virtual Status set_metadata(const RelPath& path, const Entry& metadata, MetaFields fields) = 0;

  // Removes a file, symlink or empty directory.
  virtual Status remove(const RelPath& path, EntryType type) = 0;

  virtual Result<std::unique_ptr<FileReader>> open_read(const RelPath& path) = 0;

  // Opens a file for writing. The spec tells the expected size and metadata
  // in advance for endpoints that need them at creation time.
  virtual Result<std::unique_ptr<FileWriter>> open_write(const RelPath& path,
                                                         const CommitSpec& spec) = 0;
};

}  // namespace eosmirror
