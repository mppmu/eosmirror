// SPDX-License-Identifier: GPL-3.0-or-later
#include "eosmirror/buffer.hh"

#include <algorithm>
#include <utility>

namespace eosmirror {

Buffer::Buffer(size_t size) : data_(new std::byte[size]), size_(size) {}

Buffer::Buffer(BufferPool* pool, std::unique_ptr<std::byte[]> data, size_t size)
    : pool_(pool), data_(std::move(data)), size_(size) {}

Buffer::Buffer(Buffer&& other) noexcept
    : pool_(std::exchange(other.pool_, nullptr)),
      data_(std::move(other.data_)),
      size_(std::exchange(other.size_, 0)) {}

Buffer& Buffer::operator=(Buffer&& other) noexcept {
  if (this != &other) {
    release();
    pool_ = std::exchange(other.pool_, nullptr);
    data_ = std::move(other.data_);
    size_ = std::exchange(other.size_, 0);
  }
  return *this;
}

Buffer::~Buffer() { release(); }

void Buffer::release() {
  if (pool_ && data_) pool_->give_back(std::move(data_));
  data_.reset();
  pool_ = nullptr;
  size_ = 0;
}

Buffer BufferPool::acquire() {
  std::unique_ptr<std::byte[]> data;
  {
    std::lock_guard lock(mutex_);
    if (!free_.empty()) {
      data = std::move(free_.back());
      free_.pop_back();
    } else {
      ++allocated_;
    }
  }
  if (!data) data.reset(new std::byte[buffer_size_]);
  return Buffer(this, std::move(data), buffer_size_);
}

void BufferPool::give_back(std::unique_ptr<std::byte[]> data) {
  std::lock_guard lock(mutex_);
  free_.push_back(std::move(data));
}

size_t BufferPool::allocated() const {
  std::lock_guard lock(mutex_);
  return allocated_;
}

size_t BufferPool::available() const {
  std::lock_guard lock(mutex_);
  return free_.size();
}

Chunk Chunk::copy_of(uint64_t offset, std::span<const std::byte> data) {
  Chunk chunk{offset, data.size(), Buffer(data.size())};
  std::copy(data.begin(), data.end(), chunk.buffer.span().begin());
  return chunk;
}

}  // namespace eosmirror
