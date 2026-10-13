// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "eosmirror/buffer.hh"
#include "eosmirror/checksum.hh"
#include "eosmirror/error.hh"
#include "eosmirror/types.hh"

namespace eosmirror {

// What an endpoint can do, so that the engine can adapt to it.
struct Capabilities {
  // The resolution of stored mtimes in nanoseconds (1 = full, 1000000000 = seconds).
  int32_t mtime_resolution = 1;
  // Whether entries have owners and groups (plain XRootD reports none).
  bool has_owners = true;
  // Whether owner and group can be set to arbitrary ids.
  bool can_set_owner = false;
  // Whether modes can be set, and which of the bits of files and directories
  // (plain XRootD carries only the permission bits, EOS no setuid bit on
  // directories and only the permission bits of files).
  bool can_set_mode = true;
  ModeBits file_mode_bits = 07777;
  ModeBits dir_mode_bits = 07777;
  // Whether mtimes can be set. Without it, files are compared by size only.
  bool can_set_mtime = true;
  // Whether symlinks exist on the endpoint.
  bool has_symlinks = true;
  // Whether listings and removals follow symlinks that the endpoint cannot
  // show, so that deleting through one deletes what it points to (plain
  // XRootD servers).
  bool follows_symlinks = false;
  // Whether symlinks have owners of their own that can be set.
  bool symlink_owner = true;
  // The checksum the endpoint computes itself for stored files (None if it
  // computes none), which a copy verifies against.
  ChecksumType checksum = ChecksumType::None;
  // Whether a written file is in place before its commit has confirmed it
  // (EOS's atomic uploads): a run that ends in between leaves a copy with
  // the source's size and mtime that nothing verified.
  bool confirms_after_commit = false;
};

// A directory listing. Entries that cannot be listed safely (such as lines
// of a listing that cannot be told apart) are left out and counted.
struct Listing {
  std::vector<Entry> entries;
  size_t skipped = 0;
  // A file written into the directory replaces the one there when it is
  // opened, not once it is complete (EOS directories that force uploads to
  // be non-atomic): a failed replacement leaves no file.
  bool replaces_at_open = false;
};

// Where the tree of an endpoint is, for comparing endpoints: the server
// ("host:port", empty for a local file system) and the absolute path of the
// root, resolved like realpath(3) where the endpoint can (local paths and
// EOS), else normalized.
struct Location {
  std::string server;
  std::string path;
};

// The location as "<path>", or "<path> on <server>".
std::string to_string(const Location& location);

// Whether the tree at inner is the one at outer or inside it, comparing
// whole path components.
bool contains(const Location& outer, const Location& inner);

// The URL without what may carry credentials: a password before the host and
// the opaque parameters (authz tokens and the like).
std::string display_url(std::string_view url);

// Sequential reader of one file.
class FileReader {
 public:
  virtual ~FileReader() = default;

  // Reads up to buf.size() bytes at offset; a short count means end of file.
  virtual Result<size_t> read(uint64_t offset, std::span<std::byte> buf) = 0;

  // The chunk at offset in a buffer from the pool, as long as the buffer but
  // not beyond end, and shorter only at the end of the file. Readers that
  // read ahead (XRootD) keep reading the following chunks up to end, for a
  // caller that takes them in order.
  virtual Result<Chunk> read_chunk(uint64_t offset, uint64_t end, BufferPool& pool);

  // The file as its copy starts, given its entry from the listing: the size
  // to read and the metadata for the target. The default stats the file.
  // Readers that can tell more cheaply that the file still has its listed
  // size take the listed entry (EOS, whose stat goes to the namespace).
  virtual Result<Entry> state(const Entry& listed);

  // Whether the file, once read, is still as state() found it. The default
  // stats it again and compares size and mtime.
  virtual Result<bool> unchanged(const Entry& state);

  // The file's current size and mtime.
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
  // The target may have a file of that name already, which the copy replaces.
  bool replaces = true;
};

struct CommitInfo {
  bool verified = false;  // the stored data was compared with the checksum
};

// Writer of one file to a temporary location. Nothing is visible under the
// final name until commit() succeeds.
class FileWriter {
 public:
  virtual ~FileWriter() = default;

  // Writes a chunk at the end of the data written so far. The writer may
  // keep the chunk until the write has completed, which commit() waits for.
  virtual Status write(Chunk chunk) = 0;

  // Writes a copy of data at offset.
  Status write(uint64_t offset, std::span<const std::byte> data) {
    return write(Chunk::copy_of(offset, data));
  }

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
  virtual Location location() const = 0;
  virtual Capabilities capabilities() const = 0;

  // Whether an entry is one of the endpoint's own temporaries, so that
  // listings of a target never report them as foreign. Temporaries are
  // regular files, and symlinks where the endpoint makes symlinks through
  // temporary names: a directory with the name of one is an entry like any
  // other.
  virtual bool is_temporary(const Entry& /*entry*/) const { return false; }

  // Whether a directory from a listing is on another file system than the
  // root (below a mount point); false where the endpoint cannot tell.
  virtual bool other_file_system(const Entry& /*dir*/) const { return false; }

  // A warning, and a note, about writing to this endpoint, shown when a run
  // starts with it as target; empty if there is none.
  virtual std::string target_warning() const { return {}; }
  virtual std::string target_note() const { return {}; }

  // Finds out what the endpoint can do as a target where that takes writing
  // to it (POSIX probes how precisely the file system stores mtimes), once
  // its root exists. Sources and dry runs are never probed.
  virtual void probe_target() {}

  // Makes the copies written so far durable and final where the endpoint
  // defers that (local targets, see PosixOptions::sync); a run calls it
  // once its transfers have ended.
  virtual Status flush() { return {}; }

  // Stats an entry without following symlinks. The result's name is the
  // last path component.
  virtual Result<Entry> stat(const RelPath& path) = 0;

  // Lists a directory with full metadata per entry, in no particular order.
  virtual Result<Listing> list(const RelPath& dir) = 0;

  virtual Status mkdir(const RelPath& path, ModeBits mode) = 0;

  // Creates or replaces a symlink.
  virtual Status symlink(const RelPath& path, const std::string& target) = 0;

  // Applies the selected fields of the metadata to the entry itself (not to
  // a symlink's target). Modes of symlinks are ignored.
  virtual Status set_metadata(const RelPath& path, const Entry& metadata, MetaFields fields) = 0;

  // Removes a file, symlink or empty directory.
  virtual Status remove(const RelPath& path, EntryType type) = 0;

  virtual Result<std::unique_ptr<FileReader>> open_read(const RelPath& path) = 0;

  // The checksum the endpoint stores for a file, given its entry as listed;
  // None where it stores none, or one of a type that cannot be computed.
  // EOS lists checksums with the entries, XRootD servers are asked.
  virtual Result<Checksum> stored_checksum(const RelPath& /*path*/, const Entry& listed) {
    return listed.checksum;
  }

  // Opens a file for writing. The spec tells the expected size and metadata
  // in advance for endpoints that need them at creation time.
  virtual Result<std::unique_ptr<FileWriter>> open_write(const RelPath& path,
                                                         const CommitSpec& spec) = 0;
};

inline Result<Chunk> FileReader::read_chunk(uint64_t offset, uint64_t end, BufferPool& pool) {
  Chunk chunk{offset, 0, pool.acquire()};
  uint64_t left = end > offset ? end - offset : 0;
  auto got = read(offset, chunk.buffer.span().first(
                              static_cast<size_t>(std::min<uint64_t>(chunk.buffer.size(), left))));
  if (!got.ok()) return got.error();
  chunk.size = got.value();
  return chunk;
}

inline Result<Entry> FileReader::state(const Entry& /*listed*/) { return stat(); }

inline Result<bool> FileReader::unchanged(const Entry& state) {
  auto now = stat();
  if (!now.ok()) return now.error();
  return now.value().size == state.size && now.value().mtime == state.mtime;
}

}  // namespace eosmirror
