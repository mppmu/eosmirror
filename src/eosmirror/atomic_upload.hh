// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <functional>
#include <memory>
#include <string>

#include "eosmirror/endpoint.hh"

namespace eosmirror {

// The file that an upload created, as it is now: in place under the
// upload's name (in_place), still under another name in the same directory
// (an upload not committed yet, named file.name), or elsewhere (file.name
// empty). The size, mtime and checksum are those the endpoint stores (None
// where it stores no checksum, or one of a type that cannot be computed).
struct UploadFile {
  Entry file;
  bool in_place = false;
};

// Writes a file through an upload that the endpoint renames into place
// itself when it is closed, such as EOS's atomic uploads (see
// docs/design.md), and confirms afterwards that the file in place is this
// upload: the file the upload created (known by its id), with the size and
// mtime of the copy and, where the endpoint stores one of the type computed
// while copying, its checksum.
//
// A close can report success for an upload that the storage node discarded,
// which leaves the previous file or none in place. A node can also commit
// an upload whose close failed, even later, and another writer can replace
// the file meanwhile. Since the upload carries the source's size and mtime,
// it would pass for a good copy. One that fails its verification, or whose
// close failed, is therefore taken away again: given mtime 0 where it
// replaced a file (removing it would purge the file's versions on EOS),
// removed where it replaced nothing or is still under its hidden name. Any
// other file in place is left alone, and so is one that cannot be told
// apart from the upload. Owner and mode are applied once the file is
// confirmed, and the mtime where the upload could not carry it.
class AtomicUploadWriter : public FileWriter {
 public:
  // The file that the upload created; NotFound once it is gone.
  using Locate = std::function<Result<UploadFile>()>;

  // path: the file's path on ep; name: the file for messages. The upload's
  // commit() closes it. mtime_at_open: the upload carries the mtime, which
  // the file in place must have; else it is set once the file is confirmed.
  AtomicUploadWriter(Endpoint& ep, RelPath path, std::string name,
                     std::unique_ptr<FileWriter> upload, Locate locate, bool mtime_at_open = true);

  Status write(Chunk chunk) override { return upload_->write(std::move(chunk)); }
  Result<CommitInfo> commit(const CommitSpec& spec) override;
  void abort() override { upload_->abort(); }

 private:
  // Takes the upload away where it is. Stuck: it is, or may be, in place or
  // still to be committed, and stays.
  enum class Gone { NotInPlace, TakenBack, Stuck };
  Gone take_back(const CommitSpec& spec);
  // Gives the file in place mtime 0, as a copy that is not final.
  Status reset_mtime(const CommitSpec& spec);
  // The error e of a copy whose upload was taken back: an I/O error if it
  // may stay in place, so that a journal records the copy as one in doubt.
  Error failed(Gone gone, Error e) const;
  // The error of a failed verification: that of failed(), or that the
  // upload was not committed if it is not in place.
  Error rejected(const CommitSpec& spec, Error e);
  Error not_committed() const;

  Endpoint& ep_;
  RelPath path_;
  std::string name_;
  std::unique_ptr<FileWriter> upload_;
  Locate locate_;
  bool mtime_at_open_;
};

}  // namespace eosmirror
