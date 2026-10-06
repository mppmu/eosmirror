// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>

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
  bool require_verification = false;  // fail where the target cannot verify
  std::function<void(uint64_t)> on_chunk;  // called with the size of every chunk written
  // Source reads that take longer are logged, to tell source pauses from
  // target stalls.
  std::chrono::steady_clock::duration slow_read = std::chrono::seconds(10);
};

struct CopyOutcome {
  uint64_t bytes = 0;
  Checksum checksum;
  bool verified = false;  // the target compared the stored data with the checksum
};

// Copies one file from source to target under the same relative path.
//
// The target gets the metadata and mtime the source had before reading. If
// the source's size or mtime changed by the end, nothing is committed and the
// error is Changed. Checksums are computed while streaming: the target's own
// type if it computes one, else the source's, else adler32.
Result<CopyOutcome> copy_file(Endpoint& source, Endpoint& target, const RelPath& path,
                              const CopyOptions& options, std::span<std::byte> buffer,
                              const Cancellation& cancel);

}  // namespace eosmirror
