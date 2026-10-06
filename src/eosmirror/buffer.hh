// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <span>
#include <vector>

namespace eosmirror {

class BufferPool;

// A buffer for file data. A buffer from a pool goes back to it when it is
// destroyed.
class Buffer {
 public:
  Buffer() = default;
  explicit Buffer(size_t size);  // a buffer of its own, not from a pool
  Buffer(Buffer&& other) noexcept;
  Buffer& operator=(Buffer&& other) noexcept;
  ~Buffer();

  std::span<std::byte> span() const { return {data_.get(), size_}; }
  size_t size() const { return size_; }

 private:
  friend class BufferPool;
  Buffer(BufferPool* pool, std::unique_ptr<std::byte[]> data, size_t size);
  void release();

  BufferPool* pool_ = nullptr;
  std::unique_ptr<std::byte[]> data_;
  size_t size_ = 0;
};

// The buffers of one transfer, reused for all its files. The pool allocates
// a buffer whenever none is free; the read and write windows bound how many
// a copy holds at once. All buffers must be back before the pool goes.
// Thread-safe.
class BufferPool {
 public:
  explicit BufferPool(size_t buffer_size) : buffer_size_(buffer_size) {}
  BufferPool(const BufferPool&) = delete;
  BufferPool& operator=(const BufferPool&) = delete;

  size_t buffer_size() const { return buffer_size_; }
  Buffer acquire();

  // The buffers allocated so far, and those not in use.
  size_t allocated() const;
  size_t available() const;

 private:
  friend class Buffer;
  void give_back(std::unique_ptr<std::byte[]> data);

  size_t buffer_size_;
  mutable std::mutex mutex_;
  std::vector<std::unique_ptr<std::byte[]>> free_;
  size_t allocated_ = 0;
};

// A piece of a file: size bytes at offset, held at the start of a buffer.
struct Chunk {
  uint64_t offset = 0;
  size_t size = 0;
  Buffer buffer;

  std::span<const std::byte> data() const { return buffer.span().first(size); }

  // A chunk with a copy of the data, in a buffer of its own.
  static Chunk copy_of(uint64_t offset, std::span<const std::byte> data);
};

}  // namespace eosmirror
