// SPDX-License-Identifier: GPL-3.0-or-later
#include "eosmirror/atomic_upload.hh"

#include <utility>

#include "eosmirror/log.hh"

namespace eosmirror {

AtomicUploadWriter::AtomicUploadWriter(Endpoint& ep, RelPath path, std::string name,
                                       std::unique_ptr<FileWriter> upload,
                                       ChecksumQuery stored_checksum)
    : ep_(ep),
      path_(std::move(path)),
      name_(std::move(name)),
      upload_(std::move(upload)),
      stored_checksum_(std::move(stored_checksum)) {}

Result<CommitInfo> AtomicUploadWriter::commit(const CommitSpec& spec) {
  CommitSpec upload_spec = spec;
  upload_spec.checksum = Checksum{};  // compared below
  upload_spec.require_verification = false;
  auto closed = upload_->commit(upload_spec);
  if (!closed.ok()) return failed(take_back(spec), closed.error());
  bool verified = false;
  if (spec.checksum.type != ChecksumType::None) {
    auto stored = stored_checksum_();
    if (!stored.ok())
      return stored.error().kind == ErrorKind::NotFound ? not_committed()
                                                        : rejected(spec, stored.error());
    if (stored.value().type == spec.checksum.type) {
      if (stored.value() != spec.checksum)
        return rejected(spec, Error{ErrorKind::Checksum, name_ + " has checksum " +
                                                             stored.value().hex + " instead of " +
                                                             spec.checksum.hex});
      verified = true;
    }
  }
  if (!verified) {
    // Nothing compared, so the file in place must be told by its size and
    // mtime.
    auto ours = in_place(spec);
    if (!ours.ok()) return rejected(spec, ours.error());
    if (!ours.value()) return not_committed();
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

Result<bool> AtomicUploadWriter::in_place(const CommitSpec& spec) {
  auto st = ep_.stat(path_);
  if (!st.ok()) {
    if (st.error().kind == ErrorKind::NotFound) return false;
    return st.error();
  }
  return st.value().type == EntryType::File && st.value().size == spec.size &&
         (!has(spec.fields, MetaFields::Mtime) || st.value().mtime == spec.metadata.mtime);
}

// Removes the upload, or else sets its mtime to 0 so that the next run copies
// it again. A file in place that is not the upload is left alone, and so is
// one that cannot be told apart from it.
AtomicUploadWriter::Gone AtomicUploadWriter::take_back(const CommitSpec& spec) {
  auto ours = in_place(spec);
  if (!ours.ok()) {
    log::error("cannot tell whether ", name_, " is the failed upload: ", ours.error().describe());
    return Gone::Stuck;
  }
  if (!ours.value()) return Gone::NotInPlace;
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
  }
  return e;
}

Error AtomicUploadWriter::rejected(const CommitSpec& spec, Error e) {
  Gone gone = take_back(spec);
  return gone == Gone::NotInPlace ? not_committed() : failed(gone, std::move(e));
}

// XrdCl reports the close of a file whose connection is gone as done
// (XrdClFileStateHandler.cc, Close), and the storage node then discards the
// upload.
Error AtomicUploadWriter::not_committed() const {
  return Error{ErrorKind::IO, "the upload of " + name_ +
                                  " was not committed (the connection to the storage node "
                                  "was lost before the close)"};
}

}  // namespace eosmirror
