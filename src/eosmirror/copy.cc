// SPDX-License-Identifier: GPL-3.0-or-later
#include "eosmirror/copy.hh"

#include <algorithm>

namespace eosmirror {

Result<CopyOutcome> copy_file(Endpoint& source, Endpoint& target, const RelPath& path,
                              const CopyOptions& options, std::span<std::byte> buffer,
                              const Cancellation& cancel) {
  auto opened = source.open_read(path);
  if (!opened.ok()) return opened.error();
  std::unique_ptr<FileReader> reader = std::move(opened).value();

  auto before = reader->stat();
  if (!before.ok()) return before.error();
  const Entry& src = before.value();
  if (src.type != EntryType::File)
    return Error{ErrorKind::Changed, path + " is no longer a regular file"};

  ChecksumType type = ChecksumType::None;
  if (options.verify) {
    type = target.capabilities().checksum;
    if (type == ChecksumType::None) type = source.capabilities().checksum;
    if (type == ChecksumType::None) type = ChecksumType::Adler32;
  }

  CommitSpec spec;
  spec.size = src.size;
  spec.metadata = src;
  spec.fields = MetaFields::Mtime;
  if (options.preserve_owner) spec.fields = spec.fields | MetaFields::Owner;
  if (options.preserve_mode) spec.fields = spec.fields | MetaFields::Mode;
  spec.checksum.type = type;

  auto created = target.open_write(path, spec);
  if (!created.ok()) return created.error();
  std::unique_ptr<FileWriter> writer = std::move(created).value();

  Hasher hasher(type);
  uint64_t offset = 0;
  while (offset < src.size) {
    if (cancel.requested()) {
      writer->abort();
      return Error{ErrorKind::Cancelled, "copy of " + path + " cancelled"};
    }
    size_t want = static_cast<size_t>(std::min<uint64_t>(buffer.size(), src.size - offset));
    auto got = reader->read(offset, buffer.first(want));
    if (!got.ok()) {
      writer->abort();
      return got.error();
    }
    if (got.value() == 0) {
      writer->abort();
      return Error{ErrorKind::Changed, path + " shrank during copy"};
    }
    auto chunk = buffer.first(got.value());
    hasher.update(chunk);
    Status written = writer->write(offset, chunk);
    if (!written.ok()) {
      writer->abort();
      return written.error();
    }
    offset += got.value();
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
  Status committed = writer->commit(spec);
  if (!committed.ok()) return committed.error();
  return CopyOutcome{src.size, spec.checksum};
}

}  // namespace eosmirror
