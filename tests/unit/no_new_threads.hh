// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <sys/resource.h>

#include <system_error>
#include <thread>

// Keeps the process from starting threads while it lives, by setting
// RLIMIT_NPROC to 0. The limit does not bind root outside a user namespace,
// so binds() tells whether it took effect.
class NoNewThreads {
 public:
  NoNewThreads() {
    getrlimit(RLIMIT_NPROC, &saved_);
    rlimit none = saved_;
    none.rlim_cur = 0;
    setrlimit(RLIMIT_NPROC, &none);
    try {
      std::thread([] {}).join();
    } catch (const std::system_error&) {
      binds_ = true;
    }
  }
  ~NoNewThreads() { setrlimit(RLIMIT_NPROC, &saved_); }
  NoNewThreads(const NoNewThreads&) = delete;
  NoNewThreads& operator=(const NoNewThreads&) = delete;

  bool binds() const { return binds_; }

 private:
  rlimit saved_{};
  bool binds_ = false;
};
