// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <condition_variable>
#include <deque>
#include <mutex>
#include <optional>
#include <utility>

namespace eosmirror {

// A thread-safe queue with a capacity. push() blocks while the queue is full
// and pop() while it is empty. close() releases everybody: pushes fail and
// pops return nothing once the queue has drained.
//
// With lifo = true items come out newest first, which the directory walk
// uses to stay depth first.
template <class T>
class WorkQueue {
 public:
  explicit WorkQueue(size_t capacity, bool lifo = false) : capacity_(capacity), lifo_(lifo) {}

  bool push(T item) {
    std::unique_lock lock(mutex_);
    not_full_.wait(lock, [&] { return closed_ || items_.size() < capacity_; });
    if (closed_) return false;
    items_.push_back(std::move(item));
    not_empty_.notify_one();
    return true;
  }

  std::optional<T> pop() {
    std::unique_lock lock(mutex_);
    not_empty_.wait(lock, [&] { return closed_ || !items_.empty(); });
    if (items_.empty()) return std::nullopt;
    T item;
    if (lifo_) {
      item = std::move(items_.back());
      items_.pop_back();
    } else {
      item = std::move(items_.front());
      items_.pop_front();
    }
    not_full_.notify_one();
    return item;
  }

  // Stops all waiting and future pushes. Items still queued can be popped
  // unless drain is set, in which case they are dropped.
  void close(bool drain = false) {
    std::lock_guard lock(mutex_);
    closed_ = true;
    if (drain) items_.clear();
    not_empty_.notify_all();
    not_full_.notify_all();
  }

  size_t size() const {
    std::lock_guard lock(mutex_);
    return items_.size();
  }

 private:
  mutable std::mutex mutex_;
  std::condition_variable not_empty_;
  std::condition_variable not_full_;
  std::deque<T> items_;
  size_t capacity_;
  bool lifo_;
  bool closed_ = false;
};

}  // namespace eosmirror
