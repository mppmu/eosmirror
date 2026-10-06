// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>

#include "eosmirror/buffer.hh"
#include "eosmirror/cancellation.hh"
#include "eosmirror/checksum.hh"
#include "eosmirror/endpoint.hh"
#include "eosmirror/error.hh"
#include "eosmirror/types.hh"

namespace eosmirror {

struct CopyOptions {
  bool preserve_owner = true;
  bool preserve_mode = true;
  bool preserve_mtime = true;
  bool verify = true;
  bool require_verification = false;  // fail copies that neither side can verify
  bool replaces = true;  // the target may have a file of that name already
  std::function<void(uint64_t)> on_chunk;  // called with the size of every chunk written
  // Called with the error of a failed operation on the target (open, write,
  // commit), to tell a struggling target from a struggling source.
  std::function<void(const Error&)> on_target_error;
  // Source reads that take longer are logged, to tell source pauses from
  // target stalls.
  std::chrono::steady_clock::duration slow_read = std::chrono::seconds(10);
};

struct CopyOutcome {
  uint64_t bytes = 0;
  Checksum checksum;
  bool verified = false;  // a stored checksum was compared with the data
};

// Copies one file from source to target under the same relative path, in
// chunks of the pool's buffer size.
//
// The target gets the metadata and mtime the source had before reading. If
// the source's size or mtime changed by the end, nothing is committed and the
// error is Changed. Nothing is created on the target before the first chunk
// was read. Checksums are computed while streaming: the target's own type if
// it computes one, else the source's, else adler32. The checksum the source
// stores is compared too where the target computes none, or where the
// source lists it anyway: `listed` is the source's entry from its listing,
// whose checksum counts while its size and mtime are those of the file read.
Result<CopyOutcome> copy_file(Endpoint& source, Endpoint& target, const RelPath& path,
                              const Entry& listed, const CopyOptions& options, BufferPool& pool,
                              const Cancellation& cancel);

}  // namespace eosmirror
