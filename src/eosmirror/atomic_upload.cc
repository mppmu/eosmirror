// SPDX-License-Identifier: GPL-3.0-or-later
#include "eosmirror/atomic_upload.hh"

#include <cstdio>
#include <optional>
#include <utility>

#include "eosmirror/log.hh"

namespace eosmirror {

namespace {

std::string describe_mtime(const Timespec& t) {
  char buf[40];
  std::snprintf(buf, sizeof buf, "%lld.%09d", static_cast<long long>(t.sec), t.nsec);
  return buf;
}

RelPath parent_path(const RelPath& path) {
  auto slash = path.rfind('/');
  return slash == std::string::npos ? RelPath() : path.substr(0, slash);
}

}  // namespace

AtomicUploadWriter::AtomicUploadWriter(Endpoint& ep, RelPath path, std::string name,
                                       std::unique_ptr<FileWriter> upload, Locate locate,
                                       bool mtime_at_open)
    : ep_(ep),
      path_(std::move(path)),
      name_(std::move(name)),
      upload_(std::move(upload)),
      locate_(std::move(locate)),
      mtime_at_open_(mtime_at_open) {}

Result<CommitInfo> AtomicUploadWriter::commit(const CommitSpec& spec) {
  CommitSpec upload_spec = spec;
  upload_spec.checksum = Checksum{};  // compared below
  upload_spec.require_verification = false;
  auto closed = upload_->commit(upload_spec);
  if (!closed.ok()) return failed(take_back(spec), closed.error());
  auto located = locate_();
  if (!located.ok())
    return located.error().kind == ErrorKind::NotFound ? not_committed()
                                                       : rejected(spec, located.error());
  const UploadFile& upload = located.value();
  if (!upload.in_place)
    return rejected(spec, Error{ErrorKind::IO, name_ + " is not in place after its close: the "
                                                   "upload is still " +
                                                   (upload.file.name.empty()
                                                        ? "elsewhere"
                                                        : "under " + upload.file.name)});
  const Entry& file = upload.file;
  if (file.size != spec.size)
    return rejected(spec, Error{ErrorKind::IO, name_ + " is in place with " +
                                                   std::to_string(file.size) +
                                                   " bytes instead of " +
                                                   std::to_string(spec.size)});
  bool mtime_carried = mtime_at_open_ && has(spec.fields, MetaFields::Mtime);
  if (mtime_carried && file.mtime != spec.metadata.mtime)
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
  if (!mtime_carried) fields = fields | (spec.fields & MetaFields::Mtime);
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

// Takes the upload away where it is. One that replaced a file and is in
// place gets mtime 0, so that the next attempt replaces it again, rather
// than being removed: EOS purges a file's versions with the file. One that
// replaced nothing, or is still under its hidden name (a close that timed
// out can still commit it), is removed. Where that fails, the other way is
// tried. Another file in place is left alone, and so is one that cannot be
// told apart from the upload, or that was moved elsewhere.
AtomicUploadWriter::Gone AtomicUploadWriter::take_back(const CommitSpec& spec) {
  for (int round = 0;; ++round) {
    auto located = locate_();
    if (!located.ok()) {
      if (located.error().kind == ErrorKind::NotFound) return Gone::NotInPlace;
      log::error("cannot tell whether ", name_, " is the failed upload: ",
                 located.error().describe());
      return Gone::Stuck;
    }
    const UploadFile& upload = located.value();
    if (!upload.in_place && upload.file.name.empty()) {
      log::warn("the failed upload of ", name_, " was moved elsewhere: left alone");
      return Gone::NotInPlace;
    }
    // A source mtime of 0 could not mark the file as not final.
    bool markable = upload.in_place && spec.metadata.mtime != Timespec{0, 0};
    bool keep_versions = markable && spec.replaces;
    std::optional<Error> not_reset;
    if (keep_versions) {
      Status reset = reset_mtime(spec);
      if (reset.ok()) return Gone::TakenBack;
      not_reset = reset.error();
    }
    RelPath at = upload.in_place ? path_ : join(parent_path(path_), upload.file.name);
    Status removed = ep_.remove(at, EntryType::File);
    if (removed.ok()) {
      if (not_reset)
        log::warn("removed ", name_, " after a failed upload, with its versions, since its mtime "
                  "could not be reset: ", not_reset->describe());
      return Gone::TakenBack;
    }
    // Gone since, or renamed into place by a close that went on: once more.
    if (removed.error().kind == ErrorKind::NotFound) {
      if (round == 0) continue;
      return Gone::TakenBack;
    }
    if (markable && !keep_versions) {
      Status reset = reset_mtime(spec);
      if (reset.ok()) return Gone::TakenBack;
      not_reset = reset.error();
    }
    log::error("cannot remove the failed upload of ", name_, " (", at, "): ",
               removed.error().describe(),
               not_reset ? "; nor reset its mtime: " + not_reset->describe() : "");
    return Gone::Stuck;
  }
}

Status AtomicUploadWriter::reset_mtime(const CommitSpec& spec) {
  Entry md = spec.metadata;
  md.type = EntryType::File;
  md.mtime = {0, 0};
  return ep_.set_metadata(path_, md, MetaFields::Mtime);
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
// upload. Or another writer may have replaced it since.
Error AtomicUploadWriter::not_committed() const {
  return Error{ErrorKind::IO, "the upload of " + name_ +
                                  " is not in place (the connection to the storage node was "
                                  "lost before the close, or another writer replaced it)"};
}

}  // namespace eosmirror
