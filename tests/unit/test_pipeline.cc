// SPDX-License-Identifier: GPL-3.0-or-later
#include "eosmirror/pipeline.hh"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <random>
#include <thread>
#include <vector>

#include "doctest/doctest.h"

using namespace eosmirror;

namespace {

// Completes requests from threads of its own after a random delay, so that
// they finish out of order, like XrdCl's responses.
class Completer {
 public:
  ~Completer() { join(); }

  void later(std::function<void()> f) {
    int delay;
    {
      std::lock_guard lock(mutex_);
      delay = std::uniform_int_distribution<int>(0, 2000)(random_);
    }
    std::lock_guard lock(mutex_);
    threads_.emplace_back([f = std::move(f), delay] {
      std::this_thread::sleep_for(std::chrono::microseconds(delay));
      f();
    });
  }

  void join() {
    std::vector<std::thread> threads;
    {
      std::lock_guard lock(mutex_);
      threads.swap(threads_);
    }
    for (auto& t : threads) t.join();
  }

 private:
  std::mutex mutex_;
  std::mt19937 random_{42};
  std::vector<std::thread> threads_;
};

std::vector<std::byte> pattern(size_t n) {
  std::vector<std::byte> data(n);
  for (size_t i = 0; i < n; ++i) data[i] = static_cast<std::byte>((i * 7 + i / 251) & 0xff);
  return data;
}

// Tracks how many requests are in flight at once.
struct Flight {
  std::atomic<int> now{0};
  std::atomic<int> most{0};
  void start() {
    int n = ++now;
    int m = most.load();
    while (n > m && !most.compare_exchange_weak(m, n)) {
    }
  }
  void end() { --now; }
};

}  // namespace

TEST_CASE("buffer pools reuse their buffers") {
  BufferPool pool(1000);
  {
    Buffer a = pool.acquire();
    Buffer b = pool.acquire();
    CHECK(a.size() == 1000);
    CHECK(a.span().data() != b.span().data());
    Buffer moved = std::move(a);
    CHECK(a.size() == 0);
    CHECK(pool.available() == 0);
  }
  CHECK(pool.allocated() == 2);
  CHECK(pool.available() == 2);
  for (int i = 0; i < 10; ++i) Buffer c = pool.acquire();
  CHECK(pool.allocated() == 2);

  auto data = pattern(10);
  Chunk chunk = Chunk::copy_of(5, data);
  CHECK(chunk.offset == 5);
  CHECK(chunk.size == 10);
  CHECK(std::memcmp(chunk.data().data(), data.data(), 10) == 0);
}

TEST_CASE("write windows start writes in order, contiguous and from one thread") {
  for (size_t window : {1, 3, 8}) {
    INFO("window ", window);
    Completer completer;
    Flight flight;
    BufferPool pool(100);
    std::mutex mutex;
    std::vector<uint64_t> offsets;
    std::vector<std::byte> stored;
    auto writer_thread = std::this_thread::get_id();
    bool other_thread = false;
    {
      WriteWindow writes(
          window,
          [&](const Chunk& chunk, WriteWindow::Done done) -> Status {
            flight.start();
            {
              std::lock_guard lock(mutex);
              other_thread |= std::this_thread::get_id() != writer_thread;
              offsets.push_back(chunk.offset);
              stored.resize(std::max(stored.size(), chunk.offset + chunk.size));
              std::memcpy(stored.data() + chunk.offset, chunk.data().data(), chunk.size);
            }
            completer.later([&, done = std::move(done)] {
              flight.end();
              done({});
            });
            return {};
          },
          "f");
      auto data = pattern(10000 + 37);
      for (uint64_t offset = 0; offset < data.size(); offset += 100) {
        Chunk chunk{offset, std::min<size_t>(100, data.size() - offset), pool.acquire()};
        std::memcpy(chunk.buffer.span().data(), data.data() + offset, chunk.size);
        REQUIRE(writes.write(std::move(chunk)).ok());
      }
      REQUIRE(writes.drain().ok());
      CHECK(writes.written() == data.size());
      CHECK(stored == data);
      // Out of order or with a gap, a write is refused.
      CHECK_FALSE(writes.write(Chunk{0, 1, pool.acquire()}).ok());
      CHECK_FALSE(writes.write(Chunk{data.size() + 1, 1, pool.acquire()}).ok());
    }
    CHECK_FALSE(other_thread);
    for (size_t i = 0; i < offsets.size(); ++i) CHECK(offsets[i] == i * 100);
    CHECK(flight.most <= static_cast<int>(window));
    if (window > 1) CHECK(flight.most > 1);
    CHECK(pool.available() == pool.allocated());
    CHECK(pool.allocated() <= window + 2);
  }
}

TEST_CASE("write windows stop at the first failure and report it once") {
  Completer completer;
  BufferPool pool(10);
  std::atomic<int> submitted{0};
  WriteWindow writes(
      4,
      [&](const Chunk& chunk, WriteWindow::Done done) -> Status {
        ++submitted;
        bool fail = chunk.offset == 50;
        completer.later([fail, done = std::move(done)] {
          done(fail ? Status(Error{ErrorKind::IO, "write failed"}) : Status());
        });
        return {};
      },
      "f");
  Status first;
  uint64_t offset = 0;
  for (; offset < 1000; offset += 10) {
    first = writes.write(Chunk{offset, 10, pool.acquire()});
    if (!first.ok()) break;
  }
  Status drained = writes.drain();
  REQUIRE_FALSE(drained.ok());
  CHECK(drained.error().kind == ErrorKind::IO);
  CHECK(drained.error().message.find("(last write acknowledged ") != std::string::npos);
  // Reported as it was, and nothing is started once it is known.
  if (!first.ok()) CHECK(first.error().message == drained.error().message);
  CHECK_FALSE(writes.write(Chunk{offset, 10, pool.acquire()}).ok());
  int after = submitted;
  CHECK_FALSE(writes.write(Chunk{offset, 10, pool.acquire()}).ok());
  CHECK(submitted == after);
  CHECK(pool.available() == pool.allocated());

  // A write that cannot be started ends the file as well.
  WriteWindow refused(
      2, [](const Chunk&, WriteWindow::Done) -> Status { return Error{ErrorKind::IO, "gone"}; },
      "g");
  CHECK_FALSE(refused.write(Chunk{0, 10, pool.acquire()}).ok());
  CHECK_FALSE(refused.drain().ok());
  CHECK(pool.available() == pool.allocated());
}

TEST_CASE("read ahead hands out chunks in order") {
  auto data = pattern(10000 + 37);
  for (size_t window : {1, 4}) {
    INFO("window ", window);
    Completer completer;
    Flight flight;
    BufferPool pool(100);
    std::atomic<int> started{0};
    {
      ReadAhead ahead(window, [&](uint64_t offset, std::span<std::byte> buf,
                                  ReadAhead::Done done) -> Status {
        flight.start();
        ++started;
        completer.later([&, offset, buf, done = std::move(done)] {
          size_t n = offset >= data.size() ? 0 : std::min(buf.size(), data.size() - offset);
          std::memcpy(buf.data(), data.data() + offset, n);
          flight.end();
          done(n);
        });
        return {};
      });
      std::vector<std::byte> got;
      for (uint64_t offset = 0; offset < data.size();) {
        auto chunk = ahead.next(offset, data.size(), pool);
        REQUIRE(chunk.ok());
        CHECK(chunk.value().offset == offset);
        REQUIRE(chunk.value().size > 0);
        got.insert(got.end(), chunk.value().data().begin(), chunk.value().data().end());
        offset += chunk.value().size;
      }
      CHECK(got == data);
      CHECK(started == 101);  // nothing beyond the end
      CHECK(flight.most <= static_cast<int>(window));
      if (window > 1) CHECK(flight.most > 1);

      // Another offset drops what was read ahead.
      auto again = ahead.next(500, data.size(), pool);
      REQUIRE(again.ok());
      CHECK(again.value().offset == 500);
      CHECK(std::memcmp(again.value().data().data(), data.data() + 500, 100) == 0);
    }
    // Destroyed with reads in flight: they were waited for.
    CHECK(flight.now == 0);
    CHECK(pool.available() == pool.allocated());
    CHECK(pool.allocated() <= window + 2);
  }
}

TEST_CASE("read ahead completes short reads and stops at the first error") {
  auto data = pattern(1000);
  Completer completer;
  BufferPool pool(100);
  std::atomic<int> started{0};
  // Reads return at most 30 bytes, and the one at offset 700 fails.
  ReadAhead ahead(3, [&](uint64_t offset, std::span<std::byte> buf,
                         ReadAhead::Done done) -> Status {
    ++started;
    completer.later([&, offset, buf, done = std::move(done)] {
      if (offset == 700) return done(Error{ErrorKind::IO, "read failed"});
      size_t n = std::min<size_t>({buf.size(), 30, data.size() - offset});
      std::memcpy(buf.data(), data.data() + offset, n);
      done(n);
    });
    return {};
  });
  uint64_t offset = 0;
  for (; offset < 700; offset += 100) {
    auto chunk = ahead.next(offset, data.size(), pool);
    REQUIRE(chunk.ok());
    REQUIRE(chunk.value().size == 100);
    CHECK(std::memcmp(chunk.value().data().data(), data.data() + offset, 100) == 0);
  }
  auto failed = ahead.next(700, data.size(), pool);
  REQUIRE_FALSE(failed.ok());
  CHECK(failed.error().message == "read failed");
  int after = started;
  auto again = ahead.next(800, data.size(), pool);
  REQUIRE_FALSE(again.ok());
  CHECK(again.error().message == "read failed");
  CHECK(started == after);
  ahead.stop();
  CHECK(pool.available() == pool.allocated());

  // The end of the file: a short chunk, then nothing.
  ReadAhead eof(2, [&](uint64_t off, std::span<std::byte> buf, ReadAhead::Done done) -> Status {
    size_t n = off >= 250 ? 0 : std::min<size_t>(buf.size(), 250 - off);
    done(n);  // at once, from the submitting thread
    return {};
  });
  CHECK(eof.next(200, 1000, pool).value().size == 50);
}
