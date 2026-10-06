// SPDX-License-Identifier: GPL-3.0-or-later
#include "eosmirror/queue.hh"

#include <atomic>
#include <numeric>
#include <thread>
#include <vector>

#include "doctest/doctest.h"
#include "eosmirror/cancellation.hh"
#include "eosmirror/retry.hh"

using namespace eosmirror;

TEST_CASE("work queue delivers everything once across producers and consumers") {
  WorkQueue<int> queue(4);
  const int producers = 3, consumers = 4, per_producer = 500;
  std::atomic<long> sum{0};
  std::atomic<int> received{0};

  std::vector<std::thread> threads;
  for (int c = 0; c < consumers; ++c)
    threads.emplace_back([&] {
      while (auto item = queue.pop()) {
        sum += *item;
        ++received;
      }
    });
  std::vector<std::thread> prods;
  for (int p = 0; p < producers; ++p)
    prods.emplace_back([&, p] {
      for (int i = 0; i < per_producer; ++i) REQUIRE(queue.push(p * per_producer + i));
    });
  for (auto& t : prods) t.join();
  queue.close();
  for (auto& t : threads) t.join();

  CHECK(received == producers * per_producer);
  long expected = 0;
  for (int i = 0; i < producers * per_producer; ++i) expected += i;
  CHECK(sum == expected);
}

TEST_CASE("work queue: lifo order, close with drain releases a blocked producer") {
  WorkQueue<int> stack(10, /*lifo=*/true);
  stack.push(1);
  stack.push(2);
  stack.push(3);
  CHECK(stack.pop() == 3);
  CHECK(stack.pop() == 2);

  WorkQueue<int> small(1);
  REQUIRE(small.push(1));
  std::atomic<bool> second_pushed{false};
  std::thread producer([&] { second_pushed = small.push(2); });
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  CHECK(small.size() == 1);
  small.close(/*drain=*/true);
  producer.join();
  CHECK_FALSE(second_pushed);
  CHECK_FALSE(small.pop().has_value());
}

TEST_CASE("retries follow the policy and stop on permanent errors or cancellation") {
  RetryPolicy policy;
  policy.attempts = 4;
  policy.initial_delay = std::chrono::milliseconds(1);
  policy.factor = 2;
  policy.max_delay = std::chrono::milliseconds(3);
  CHECK(policy.delay(1) == std::chrono::milliseconds(1));
  CHECK(policy.delay(2) == std::chrono::milliseconds(2));
  CHECK(policy.delay(3) == std::chrono::milliseconds(3));  // capped

  Cancellation cancel;
  std::atomic<uint64_t> retries{0};

  int calls = 0;
  Status s = with_retries(policy, cancel, retries, [&] {
    ++calls;
    return calls < 3 ? Status(Error{ErrorKind::IO, "flaky"}) : Status();
  });
  CHECK(s.ok());
  CHECK(calls == 3);
  CHECK(retries == 2);

  calls = 0;
  s = with_retries(policy, cancel, retries, [&] {
    ++calls;
    return Status(Error{ErrorKind::NotFound, "gone"});
  });
  CHECK_FALSE(s.ok());
  CHECK(calls == 1);

  calls = 0;
  s = with_retries(policy, cancel, retries, [&] {
    ++calls;
    return Status(Error{ErrorKind::IO, "always"});
  });
  CHECK(calls == 4);
  CHECK(retries == 5);

  cancel.request();
  calls = 0;
  s = with_retries(policy, cancel, retries, [&] {
    ++calls;
    return Status(Error{ErrorKind::IO, "always"});
  });
  CHECK(calls == 1);
  CHECK(cancel.wait(std::chrono::milliseconds(1000)));
}
