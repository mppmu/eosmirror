// SPDX-License-Identifier: GPL-3.0-or-later
#include "eosmirror/log.hh"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <mutex>

namespace eosmirror {

namespace {

// The length of the valid UTF-8 encoding of a printable character (U+00A0
// and above) at the start of s, or 0.
size_t utf8_printable(std::string_view s) {
  auto lead = static_cast<unsigned char>(s[0]);
  size_t len = lead >= 0xf8 ? 0 : lead >= 0xf0 ? 4 : lead >= 0xe0 ? 3 : lead >= 0xc0 ? 2 : 0;
  if (len == 0 || len > s.size()) return 0;
  uint32_t code = lead & (0x3f >> (len - 1));
  for (size_t i = 1; i < len; ++i) {
    auto c = static_cast<unsigned char>(s[i]);
    if ((c & 0xc0) != 0x80) return 0;
    code = code << 6 | (c & 0x3f);
  }
  static constexpr uint32_t kShortest[] = {0, 0, 0x80, 0x800, 0x10000};
  if (code < kShortest[len] || code < 0xa0 || code > 0x10ffff || (code >= 0xd800 && code < 0xe000))
    return 0;
  return len;
}

}  // namespace

std::string printable(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  for (size_t i = 0; i < text.size();) {
    auto c = static_cast<unsigned char>(text[i]);
    if (c >= 0x20 && c < 0x7f) {
      out += static_cast<char>(c);
      ++i;
    } else if (size_t len = c >= 0x80 ? utf8_printable(text.substr(i)) : 0; len > 0) {
      out.append(text.substr(i, len));
      i += len;
    } else {
      out += '?';
      ++i;
    }
  }
  return out;
}

}  // namespace eosmirror

namespace eosmirror::log {

namespace {

std::atomic<LogLevel> g_level{LogLevel::Info};
std::mutex g_mutex;
std::function<void()> g_erase;
std::function<void()> g_redraw;

const char* prefix(LogLevel level) {
  switch (level) {
    case LogLevel::Error: return "ERROR";
    case LogLevel::Warn: return "WARN";
    case LogLevel::Info: return "INFO";
    case LogLevel::Debug: return "DEBUG";
  }
  return "";
}

}  // namespace

void set_level(LogLevel level) { g_level.store(level, std::memory_order_relaxed); }
LogLevel level() { return g_level.load(std::memory_order_relaxed); }

void write(LogLevel level, std::string_view message) {
  auto now = std::chrono::system_clock::now();
  std::time_t t = std::chrono::system_clock::to_time_t(now);
  std::tm tm{};
  localtime_r(&t, &tm);
  char stamp[32];
  std::strftime(stamp, sizeof stamp, "%Y-%m-%d %H:%M:%S", &tm);
  std::string text = printable(message);
  std::lock_guard lock(g_mutex);
  if (g_erase) g_erase();
  std::fprintf(stderr, "%s %-5s %s\n", stamp, prefix(level), text.c_str());
  if (g_redraw) g_redraw();
}

std::unique_lock<std::mutex> lock() { return std::unique_lock<std::mutex>(g_mutex); }

void set_display_hooks(std::function<void()> erase, std::function<void()> redraw) {
  std::lock_guard lock(g_mutex);
  g_erase = std::move(erase);
  g_redraw = std::move(redraw);
}

}  // namespace eosmirror::log
