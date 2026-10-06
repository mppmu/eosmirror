// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "eosmirror/buffer.hh"
#include "eosmirror/error.hh"

namespace eosmirror {

// Reads of a file's chunks kept in flight ahead of a reader that takes them
// in order: the transport-independent part of pipelined reads. All reads are
// started from the thread that calls next().
class ReadAhead {
 public:
  // Called exactly once with the number of bytes read or the error, from
  // any thread. Fewer bytes than asked for mean the end of the file.
  using Done = std::function<void(Result<size_t>)>;
  // Starts a read into buf. An error means that it did not start, and that
  // done is never called.
  using Submit = std::function<Status(uint64_t offset, std::span<std::byte> buf, Done done)>;

  ReadAhead(size_t window, Submit submit)
      : window_(std::max<size_t>(window, 1)), submit_(std::move(submit)) {}
  ReadAhead(const ReadAhead&) = delete;
  ReadAhead& operator=(const ReadAhead&) = delete;
  ~ReadAhead() { stop(); }

  // The chunk at offset, of the pool's buffer size but not beyond end, while
  // up to `window` chunks are being read from there on. Asking for another
  // offset than the one after the previous chunk drops what was read ahead.
  // After an error, nothing more is read and the error is returned again.
  Result<Chunk> next(uint64_t offset, uint64_t end, BufferPool& pool);

  // Waits for the reads in flight and drops what was read ahead.
  void stop();

 private:
  struct Slot {
    uint64_t offset = 0;
    Buffer buffer;
    size_t want = 0;
    std::optional<Result<size_t>> result;  // set on completion
  };

  // Starts reads up to the window and end.
  void fill(std::unique_lock<std::mutex>& lock, uint64_t end, BufferPool& pool);
  // Starts a read into buf for a slot and waits for nothing; the slot gets
  // the result. The lock is released while the read is submitted.
  void start(std::unique_lock<std::mutex>& lock, Slot& slot, std::span<std::byte> buf);
  // Waits for the reads in flight and drops all slots.
  void drop(std::unique_lock<std::mutex>& lock);

  const size_t window_;
  Submit submit_;
  std::mutex mutex_;
  std::condition_variable cv_;
  std::deque<std::unique_ptr<Slot>> slots_;  // in offset order
  int in_flight_ = 0;
  uint64_t next_offset_ = 0;  // of the next read to start
  bool refused_ = false;      // a read ahead could not be started
  std::optional<Error> error_;
};

// Writes of one file kept in flight: the transport-independent part of
// pipelined writes. Writes are started from the thread that calls write(),
// each at the end of the previous one, and once a write has failed, no
// further write is started.
class WriteWindow {
 public:
  // Called exactly once when the write completes, from any thread.
  using Done = std::function<void(Status)>;
  // Starts writing a chunk. An error means that it did not start, and that
  // done is never called.
  using Submit = std::function<Status(const Chunk& chunk, Done done)>;

  // name: the file, for messages.
  WriteWindow(size_t window, Submit submit, std::string name);
  WriteWindow(const WriteWindow&) = delete;
  WriteWindow& operator=(const WriteWindow&) = delete;
  ~WriteWindow() { (void)drain(); }

  // Waits until fewer than `window` writes are in flight and starts writing
  // the chunk, which must begin where the previous one ended. The chunk's
  // buffer is released when its write completes.
  Status write(Chunk chunk);

  // Waits until no write is in flight; the first error, if any.
  Status drain();

  // The bytes handed to write() so far.
  uint64_t written() const { return written_; }

  // Adds how long ago the server last acknowledged a write, which tells a
  // stalled connection from an immediate refusal.
  Error stalled(Error e) const;

 private:
  void completed(size_t slot, Status status);
  Error stalled_locked(Error e) const;

  const std::string name_;
  Submit submit_;
  mutable std::mutex mutex_;
  std::condition_variable cv_;
  std::vector<std::optional<Chunk>> slots_;  // the chunks in flight
  int in_flight_ = 0;
  std::optional<Error> error_;
  uint64_t written_ = 0;  // only touched by the writing thread
  std::chrono::steady_clock::time_point last_ack_ = std::chrono::steady_clock::now();
};

}  // namespace eosmirror
