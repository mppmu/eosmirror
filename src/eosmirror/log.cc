// SPDX-License-Identifier: GPL-3.0-or-later
#include "eosmirror/log.hh"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <mutex>

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
  std::lock_guard lock(g_mutex);
  if (g_erase) g_erase();
  std::fprintf(stderr, "%s %-5s %.*s\n", stamp, prefix(level), static_cast<int>(message.size()),
               message.data());
  if (g_redraw) g_redraw();
}

std::unique_lock<std::mutex> lock() { return std::unique_lock<std::mutex>(g_mutex); }

void set_display_hooks(std::function<void()> erase, std::function<void()> redraw) {
  std::lock_guard lock(g_mutex);
  g_erase = std::move(erase);
  g_redraw = std::move(redraw);
}

}  // namespace eosmirror::log
