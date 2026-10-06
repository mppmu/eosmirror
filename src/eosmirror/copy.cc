// SPDX-License-Identifier: GPL-3.0-or-later
#include "eosmirror/copy.hh"

#include <algorithm>
#include <cstdio>
#include <optional>

#include "eosmirror/log.hh"

namespace eosmirror {

Result<CopyOutcome> copy_file(Endpoint& source, Endpoint& target, const RelPath& path,
                              const Entry& listed, const CopyOptions& options, BufferPool& pool,
                              const Cancellation& cancel) {
  auto opened = source.open_read(path);
  if (!opened.ok()) return opened.error();
  std::unique_ptr<FileReader> reader = std::move(opened).value();

  auto before = reader->stat();
  if (!before.ok()) return before.error();
  const Entry& src = before.value();
  if (src.type != EntryType::File)
    return Error{ErrorKind::Changed, path + " is no longer a regular file"};

  ChecksumType target_type = target.capabilities().checksum;
  ChecksumType type = ChecksumType::None;
  if (options.verify) {
    type = target_type;
    if (type == ChecksumType::None) type = source.capabilities().checksum;
    if (type == ChecksumType::None) type = ChecksumType::Adler32;
  }

  CommitSpec spec;
  spec.size = src.size;
  spec.metadata = src;
  spec.fields = MetaFields::None;
  if (options.preserve_mtime) spec.fields = spec.fields | MetaFields::Mtime;
  if (options.preserve_owner) spec.fields = spec.fields | MetaFields::Owner;
  if (options.preserve_mode) spec.fields = spec.fields | MetaFields::Mode;
  spec.checksum.type = type;
  spec.replaces = options.replaces;

  // The chunk at offset, which ends short only where the file does.
  auto read = [&](uint64_t offset) -> Result<Chunk> {
    size_t want = static_cast<size_t>(std::min<uint64_t>(pool.buffer_size(), src.size - offset));
    auto started = std::chrono::steady_clock::now();
    auto chunk = reader->read_chunk(offset, src.size, pool);
    auto took = std::chrono::steady_clock::now() - started;
    if (took > options.slow_read) {
      char secs[32];
      std::snprintf(secs, sizeof secs, "%.1f", std::chrono::duration<double>(took).count());
      log::warn("slow source read: ", path, " at offset ", offset, ", ", want, " bytes, ", secs,
                " s");
    }
    if (chunk.ok() && chunk.value().size < want)
      return Error{ErrorKind::Changed, path + " shrank during copy"};
    return chunk;
  };

  // The first chunk is read before the target is touched: an unreadable
  // source leaves nothing behind there, and reading ahead overlaps the open.
  std::optional<Chunk> first;
  if (src.size > 0) {
    auto chunk = read(0);
    if (!chunk.ok()) return chunk.error();
    first = std::move(chunk).value();
  }

  // An error of an operation on the target, reported as such.
  auto target_failed = [&](const Error& e) {
    if (options.on_target_error) options.on_target_error(e);
    return e;
  };

  auto created = target.open_write(path, spec);
  if (!created.ok()) return target_failed(created.error());
  std::unique_ptr<FileWriter> writer = std::move(created).value();

  Hasher hasher(type);
  uint64_t offset = 0;
  while (offset < src.size) {
    if (cancel.requested()) {
      writer->abort();
      return Error{ErrorKind::Cancelled, "copy of " + path + " cancelled"};
    }
    Chunk chunk;
    if (first) {
      chunk = std::move(*first);
      first.reset();
    } else {
      auto next = read(offset);
      if (!next.ok()) {
        writer->abort();
        return next.error();
      }
      chunk = std::move(next).value();
    }
    size_t size = chunk.size;
    hasher.update(chunk.data());
    Status written = writer->write(std::move(chunk));
    if (!written.ok()) {
      writer->abort();
      return target_failed(written.error());
    }
    if (options.on_chunk) options.on_chunk(size);
    offset += size;
  }

  auto after = reader->stat();
  if (!after.ok()) {
    writer->abort();
    return after.error();
  }
  if (after.value().size != src.size || after.value().mtime != src.mtime) {
    writer->abort();
    return Error{ErrorKind::Changed, path + " changed during copy"};
  }
  spec.checksum = hasher.finish();

  // The source's stored checksum is compared where the target computes
  // none, and where the listing has it anyway (EOS). One from the listing
  // belongs to the data read only while the file still has the listed size
  // and mtime.
  bool source_verified = false;
  Entry known = listed;
  if (listed.size != src.size || listed.mtime != src.mtime) known.checksum = {};
  if (type != ChecksumType::None &&
      (target_type == ChecksumType::None || known.checksum.type == type)) {
    auto stored = source.stored_checksum(path, known);
    if (!stored.ok()) {
      writer->abort();
      return stored.error();
    }
    if (stored.value().type == type) {
      if (stored.value() != spec.checksum) {
        writer->abort();
        return Error{ErrorKind::Checksum, path + " has checksum " + stored.value().hex +
                                              " on the source, but " + spec.checksum.hex +
                                              " was read"};
      }
      source_verified = true;
    }
  }
  spec.require_verification = options.require_verification && !source_verified;
  auto committed = writer->commit(spec);
  if (!committed.ok()) return target_failed(committed.error());
  return CopyOutcome{src.size, spec.checksum, committed.value().verified || source_verified};
}

}  // namespace eosmirror
