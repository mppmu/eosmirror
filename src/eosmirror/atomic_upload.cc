// SPDX-License-Identifier: GPL-3.0-or-later
#include "eosmirror/atomic_upload.hh"

#include <cstdio>
#include <utility>

#include "eosmirror/log.hh"

namespace eosmirror {

namespace {

std::string describe_mtime(const Timespec& t) {
  char buf[40];
  std::snprintf(buf, sizeof buf, "%lld.%09d", static_cast<long long>(t.sec), t.nsec);
  return buf;
}

}  // namespace

AtomicUploadWriter::AtomicUploadWriter(Endpoint& ep, RelPath path, std::string name,
                                       std::unique_ptr<FileWriter> upload, Placed placed)
    : ep_(ep),
      path_(std::move(path)),
      name_(std::move(name)),
      upload_(std::move(upload)),
      placed_(std::move(placed)) {}

Result<CommitInfo> AtomicUploadWriter::commit(const CommitSpec& spec) {
  CommitSpec upload_spec = spec;
  upload_spec.checksum = Checksum{};  // compared below
  upload_spec.require_verification = false;
  auto closed = upload_->commit(upload_spec);
  if (!closed.ok()) return failed(take_back(spec), closed.error());
  auto placed = placed_();
  if (!placed.ok())
    return placed.error().kind == ErrorKind::NotFound ? not_committed()
                                                      : rejected(spec, placed.error());
  const Entry& file = placed.value();
  if (file.size != spec.size)
    return rejected(spec, Error{ErrorKind::IO, name_ + " is in place with " +
                                                   std::to_string(file.size) +
                                                   " bytes instead of " +
                                                   std::to_string(spec.size)});
  if (has(spec.fields, MetaFields::Mtime) && file.mtime != spec.metadata.mtime)
    return rejected(spec, Error{ErrorKind::IO, name_ + " is in place with mtime " +
                                                   describe_mtime(file.mtime) + " instead of " +
                                                   describe_mtime(spec.metadata.mtime)});
  bool verified = false;
  if (spec.checksum.type != ChecksumType::None && file.checksum.type == spec.checksum.type) {
    if (file.checksum != spec.checksum)
      return rejected(spec, Error{ErrorKind::Checksum, name_ + " has checksum " +
                                                           file.checksum.hex + " instead of " +
                                                           spec.checksum.hex});
    verified = true;
  }
  if (spec.require_verification && !verified)
    return rejected(spec, Error{ErrorKind::Unsupported,
                                ep_.describe() + " stores no " +
                                    std::string(to_string(spec.checksum.type)) +
                                    " checksum to verify " + name_ + " against"});
  MetaFields fields = spec.fields & (MetaFields::Owner | MetaFields::Mode);
  // A new file has the mode of the open, plus the owner's read and write
  // bits that XRootD adds. One that replaced a file may have been given the
  // old file's mode by EOS's versioning.
  if (!spec.replaces && (spec.metadata.mode & 0600) == 0600) fields = fields & ~MetaFields::Mode;
  Entry md = spec.metadata;
  md.type = EntryType::File;
  Status set = ep_.set_metadata(path_, md, fields);
  if (!set.ok()) return set.error();
  return CommitInfo{verified};
}

// Removes the upload, or else sets its mtime to 0 so that the next run copies
// it again. Another file in place is left alone, and so is one that cannot
// be told apart from the upload.
AtomicUploadWriter::Gone AtomicUploadWriter::take_back(const CommitSpec& spec) {
  auto placed = placed_();
  if (!placed.ok()) {
    if (placed.error().kind == ErrorKind::NotFound) return Gone::NotInPlace;
    log::error("cannot tell whether ", name_, " is the failed upload: ",
               placed.error().describe());
    return Gone::Stuck;
  }
  Status removed = ep_.remove(path_, EntryType::File);
  if (removed.ok() || removed.error().kind == ErrorKind::NotFound) return Gone::TakenBack;
  Entry md = spec.metadata;
  md.type = EntryType::File;
  md.mtime = {0, 0};
  Status reset = ep_.set_metadata(path_, md, MetaFields::Mtime);
  if (reset.ok()) return Gone::TakenBack;
  log::error("cannot remove ", name_, " after a failed upload, nor reset its mtime: ",
             removed.error().describe(), "; ", reset.error().describe());
  return Gone::Stuck;
}

Error AtomicUploadWriter::failed(Gone gone, Error e) const {
  if (gone == Gone::Stuck) {
    e.kind = ErrorKind::IO;
    e.message += "; " + name_ + " may stay in place unverified";
    e.in_doubt = true;
  }
  return e;
}

Error AtomicUploadWriter::rejected(const CommitSpec& spec, Error e) {
  Gone gone = take_back(spec);
  return gone == Gone::NotInPlace ? not_committed() : failed(gone, std::move(e));
}

// XrdCl reports the close of a file whose connection is gone as done
// (XrdClFileStateHandler.cc, Close), and the storage node then discards the
// upload; or another writer replaced it since.
Error AtomicUploadWriter::not_committed() const {
  return Error{ErrorKind::IO, "the upload of " + name_ +
                                  " is not in place (the connection to the storage node was "
                                  "lost before the close, or another writer replaced it)"};
}

}  // namespace eosmirror
