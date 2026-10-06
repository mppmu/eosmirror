// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <functional>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>

namespace eosmirror {

enum class LogLevel : int { Error = 0, Warn = 1, Info = 2, Debug = 3 };

// Thread-safe logging to stderr. Messages below the configured level are
// dropped before their arguments are formatted.
namespace log {

void set_level(LogLevel level);
LogLevel level();
inline bool enabled(LogLevel l) { return l <= level(); }

void write(LogLevel level, std::string_view message);

// The mutex that serializes output; a terminal display renders under it.
std::unique_lock<std::mutex> lock();

// A terminal display registers how to step aside before a log line is
// written and how to redraw afterwards. Both are called under the mutex.
void set_display_hooks(std::function<void()> erase, std::function<void()> redraw);

template <class... Args>
void emit(LogLevel l, const Args&... args) {
  if (!enabled(l)) return;
  std::ostringstream s;
  (s << ... << args);
  write(l, s.str());
}

template <class... Args>
void error(const Args&... args) {
  emit(LogLevel::Error, args...);
}
template <class... Args>
void warn(const Args&... args) {
  emit(LogLevel::Warn, args...);
}
template <class... Args>
void info(const Args&... args) {
  emit(LogLevel::Info, args...);
}
template <class... Args>
void debug(const Args&... args) {
  emit(LogLevel::Debug, args...);
}

}  // namespace log
}  // namespace eosmirror
