// SPDX-License-Identifier: GPL-3.0-or-later
#include "eosmirror/pipeline.hh"

namespace eosmirror {

// ---- ReadAhead --------------------------------------------------------------------

Result<Chunk> ReadAhead::next(uint64_t offset, uint64_t end, BufferPool& pool) {
  std::unique_lock lock(mutex_);
  if (error_) return *error_;
  if (!slots_.empty() && slots_.front()->offset != offset) drop(lock);
  if (slots_.empty()) next_offset_ = offset;
  fill(lock, end, pool);
  if (slots_.empty()) return Chunk{offset, 0, {}};  // at or beyond the end

  Slot& front = *slots_.front();
  cv_.wait(lock, [&] { return front.result.has_value(); });
  Result<size_t> result = std::move(*front.result);
  // A read ends short only at the end of the file, but servers are not
  // relied upon for that: the rest is asked for until a read returns nothing.
  if (result.ok()) {
    size_t got = result.value(), last = got;
    while (last > 0 && got < front.want) {
      Slot rest;
      rest.offset = front.offset + got;
      rest.want = front.want - got;
      start(lock, rest, front.buffer.span().subspan(got, rest.want));
      cv_.wait(lock, [&] { return rest.result.has_value(); });
      if (!rest.result->ok()) {
        result = rest.result->error();
        break;
      }
      last = rest.result->value();
      got += last;
      result = got;
    }
  }

  Chunk chunk{front.offset, result.ok() ? result.value() : 0, std::move(front.buffer)};
  slots_.pop_front();
  if (!result.ok()) {
    error_ = result.error();  // nothing more is started
    return result.error();
  }
  fill(lock, end, pool);
  return chunk;
}

void ReadAhead::fill(std::unique_lock<std::mutex>& lock, uint64_t end, BufferPool& pool) {
  while (!error_ && !refused_ && slots_.size() < window_ && next_offset_ < end) {
    auto slot = std::make_unique<Slot>();
    slot->offset = next_offset_;
    slot->want = static_cast<size_t>(std::min<uint64_t>(pool.buffer_size(), end - next_offset_));
    slot->buffer = pool.acquire();
    next_offset_ += slot->want;
    Slot& s = *slot;
    slots_.push_back(std::move(slot));
    start(lock, s, s.buffer.span().first(s.want));
    refused_ = s.result && !s.result->ok();  // not started: next() reports it
  }
}

void ReadAhead::start(std::unique_lock<std::mutex>& lock, Slot& slot, std::span<std::byte> buf) {
  ++in_flight_;
  Done done = [this, &slot](Result<size_t> result) {
    std::lock_guard guard(mutex_);
    slot.result = std::move(result);
    --in_flight_;
    cv_.notify_all();
  };
  // Released while submitting, in case a completion comes at once; only
  // this thread adds or removes slots.
  lock.unlock();
  Status started = submit_(slot.offset, buf, std::move(done));
  lock.lock();
  if (!started.ok()) {
    slot.result = started.error();
    --in_flight_;
  }
}

void ReadAhead::drop(std::unique_lock<std::mutex>& lock) {
  cv_.wait(lock, [&] { return in_flight_ == 0; });
  slots_.clear();
  refused_ = false;
}

void ReadAhead::stop() {
  std::unique_lock lock(mutex_);
  drop(lock);
}

// ---- WriteWindow ------------------------------------------------------------------

WriteWindow::WriteWindow(size_t window, Submit submit, std::string name)
    : name_(std::move(name)), submit_(std::move(submit)), slots_(std::max<size_t>(window, 1)) {}

Status WriteWindow::write(Chunk chunk) {
  size_t size = chunk.size;
  std::unique_lock lock(mutex_);
  if (!error_ && chunk.offset != written_)
    return Error{ErrorKind::Other, "non-sequential write to " + name_};
  cv_.wait(lock, [&] { return error_ || in_flight_ < static_cast<int>(slots_.size()); });
  if (error_) return *error_;
  size_t index = 0;
  while (slots_[index]) ++index;
  slots_[index] = std::move(chunk);
  ++in_flight_;
  const Chunk& submitted = *slots_[index];
  lock.unlock();
  Status started =
      submit_(submitted, [this, index](Status status) { completed(index, std::move(status)); });
  if (!started.ok()) {
    lock.lock();
    slots_[index].reset();
    --in_flight_;
    if (!error_) error_ = stalled_locked(started.error());
    cv_.notify_all();
    return *error_;
  }
  written_ += size;
  return {};
}

void WriteWindow::completed(size_t index, Status status) {
  std::lock_guard lock(mutex_);
  if (status.ok())
    last_ack_ = std::chrono::steady_clock::now();
  else if (!error_)
    error_ = stalled_locked(status.error());
  // The buffer goes back to its pool before drain() can return.
  slots_[index].reset();
  --in_flight_;
  cv_.notify_all();
}

Status WriteWindow::drain() {
  std::unique_lock lock(mutex_);
  cv_.wait(lock, [&] { return in_flight_ == 0; });
  if (error_) return *error_;
  return {};
}

Error WriteWindow::stalled(Error e) const {
  std::lock_guard lock(mutex_);
  return stalled_locked(std::move(e));
}

Error WriteWindow::stalled_locked(Error e) const {
  auto secs = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() -
                                                               last_ack_)
                  .count();
  e.message += " (last write acknowledged " + std::to_string(secs) + " s earlier)";
  return e;
}

}  // namespace eosmirror
