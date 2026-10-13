// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <functional>
#include <memory>
#include <string>

#include "eosmirror/endpoint.hh"

namespace eosmirror {

// Writes a file through an upload that the endpoint renames into place
// itself when it is closed, such as EOS's atomic uploads (see
// docs/design.md), and confirms afterwards that the file in place is this
// upload: the file the upload created (known by its id), with the size and
// mtime of the copy and, where the endpoint stores one of the type computed
// while copying, its checksum. A close can report success for an upload
// that the storage node discarded, which leaves the previous file or none
// in place; a node can commit an upload whose close failed; and another
// writer can replace the file meanwhile. The upload carries the source's
// size and mtime, so that it would pass for a good copy: one that fails its
// verification, or whose close failed, is taken away again if it is in
// place, removed or else given mtime 0. Any other file in place is left
// alone, and so is one that cannot be told apart from the upload. Owner and
// mode are applied once the file is confirmed.
class AtomicUploadWriter : public FileWriter {
 public:
  // The file that the upload created as it is now (size, mtime, and the
  // checksum the endpoint stores, None if it stores none or one of a type
  // that cannot be computed); NotFound unless it is in place under its name.
  using Placed = std::function<Result<Entry>()>;

  // path: the file's path on ep; name: the file for messages. The upload's
  // commit() closes it.
  AtomicUploadWriter(Endpoint& ep, RelPath path, std::string name,
                     std::unique_ptr<FileWriter> upload, Placed placed);

  Status write(Chunk chunk) override { return upload_->write(std::move(chunk)); }
  Result<CommitInfo> commit(const CommitSpec& spec) override;
  void abort() override { upload_->abort(); }

 private:
  // Takes the upload away if it is in place. Stuck: it is, or may be, and
  // stays.
  enum class Gone { NotInPlace, TakenBack, Stuck };
  Gone take_back(const CommitSpec& spec);
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
  Placed placed_;
};

}  // namespace eosmirror
