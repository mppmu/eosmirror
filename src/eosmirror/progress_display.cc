// SPDX-License-Identifier: GPL-3.0-or-later
#include "eosmirror/progress_display.hh"

#include <sys/ioctl.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <sstream>

#include "eosmirror/log.hh"

namespace eosmirror {

namespace {

constexpr int kBarWidth = 20;
constexpr int kMaxTransferLines = 12;

// A bar of the given width in cells, filled to the ratio with block
// characters, with eighth-block resolution.
std::string bar(double ratio, int width) {
  static const char* const kEighths[] = {"", "▏", "▎", "▍", "▌", "▋", "▊", "▉"};
  ratio = std::clamp(ratio, 0.0, 1.0);
  int eighths = static_cast<int>(ratio * width * 8 + 0.5);
  int full = eighths / 8;
  int partial = eighths % 8;
  std::string s = "▕";
  for (int i = 0; i < full; ++i) s += "█";
  int used = full;
  if (partial > 0 && used < width) {
    s += kEighths[partial];
    ++used;
  }
  for (int i = used; i < width; ++i) s += "░";
  return s + "▏";
}

std::string fit(const std::string& text, size_t width) {
  if (text.size() <= width) return text;
  if (width < 4) return text.substr(0, width);
  return "…" + text.substr(text.size() - width + 1);
}

std::string rate(uint64_t bytes, double seconds) {
  return format_bytes(seconds > 0 ? static_cast<uint64_t>(static_cast<double>(bytes) / seconds)
                                  : 0) +
         "/s";
}

}  // namespace

ProgressDisplay::ProgressDisplay(const Report& report, std::string title)
    : report_(report), title_(std::move(title)) {}

ProgressDisplay::~ProgressDisplay() { stop(); }

bool ProgressDisplay::suitable(int fd) {
  if (!isatty(fd)) return false;
  const char* term = getenv("TERM");
  return term && *term && std::string(term) != "dumb";
}

void ProgressDisplay::start() {
  if (thread_.joinable()) return;
  log::set_display_hooks([this] { erase(); }, [this] { render(); });
  thread_ = std::thread([this] { loop(); });
}

void ProgressDisplay::stop() {
  if (!thread_.joinable()) return;
  stop_ = true;
  thread_.join();
  {
    auto lock = log::lock();
    erase();
  }
  log::set_display_hooks(nullptr, nullptr);
}

void ProgressDisplay::loop() {
  while (!stop_) {
    {
      auto lock = log::lock();
      render();
    }
    for (int i = 0; i < 5 && !stop_; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
}

// Both run under the log mutex, so that log lines and the display never
// interleave: the lines are rewritten in place, the cursor stays at the top.
void ProgressDisplay::render() {
  struct winsize ws{};
  if (ioctl(STDERR_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 20) width_ = ws.ws_col;
  std::string text = render_lines();
  int lines = static_cast<int>(std::count(text.begin(), text.end(), '\n'));
  std::string out;
  std::istringstream in(text);
  std::string line;
  while (std::getline(in, line)) out += "\r\x1b[K" + line + "\n";
  // Blank any lines the previous display had beyond this one.
  for (int i = lines; i < lines_shown_; ++i) out += "\r\x1b[K\n";
  int total = std::max(lines, lines_shown_);
  if (total > 0) out += "\x1b[" + std::to_string(total) + "A";
  std::fputs(out.c_str(), stderr);
  std::fflush(stderr);
  lines_shown_ = lines;
}

void ProgressDisplay::erase() {
  if (lines_shown_ == 0) return;
  std::string out;
  for (int i = 0; i < lines_shown_; ++i) out += "\r\x1b[K\n";
  out += "\x1b[" + std::to_string(lines_shown_) + "A";
  std::fputs(out.c_str(), stderr);
  std::fflush(stderr);
  lines_shown_ = 0;
}

std::string ProgressDisplay::render_lines() {
  const Stats& s = report_.stats;
  double secs = std::chrono::duration<double>(report_.elapsed()).count();
  std::ostringstream out;
  out << fit(title_, static_cast<size_t>(width_ - 20)) << "  " << format_duration(report_.elapsed())
      << "\n";
  out << "dirs " << s.dirs_listed.load() << "  files " << s.files_checked.load() << " checked, "
      << s.files_copied.load() << " copied (" << format_bytes(s.bytes_copied.load()) << "), "
      << s.files_unchanged.load() << " unchanged";
  if (s.symlinks_created.load() + s.symlinks_unchanged.load() > 0)
    out << "  links " << s.symlinks_created.load() + s.symlinks_unchanged.load();
  if (s.metadata_fixed.load() > 0) out << "  fixed " << s.metadata_fixed.load();
  if (s.deleted.load() > 0) out << "  deleted " << s.deleted.load();
  out << "  failed " << s.failures.load();
  if (s.retries.load() > 0) out << "  retries " << s.retries.load();
  out << "\n";

  size_t active = 0;
  for (const auto& slot : report_.slots)
    if (slot->active.load()) ++active;
  size_t slots = report_.slots.size();
  size_t queued = s.queued_copies.load();
  size_t backlog_max = std::max<size_t>(report_.backlog_capacity, 1);
  out << "written " << format_bytes(s.bytes_written.load()) << " at "
      << rate(s.bytes_written.load(), secs) << "   transfers "
      << bar(slots ? static_cast<double>(active) / static_cast<double>(slots) : 0, kBarWidth / 2)
      << " " << active << "/" << slots << "   backlog "
      << bar(static_cast<double>(queued) / static_cast<double>(backlog_max), kBarWidth / 2) << " "
      << queued << "\n";

  int shown = 0;
  for (const auto& slot : report_.slots) {
    if (!slot->active.load()) continue;
    if (shown++ >= kMaxTransferLines) break;
    std::string path;
    uint64_t size;
    {
      std::lock_guard lock(slot->mutex);
      path = slot->path;
      size = slot->size;
    }
    uint64_t written = slot->written.load();
    double ratio = size ? static_cast<double>(written) / static_cast<double>(size) : 1.0;
    char pct[8];
    std::snprintf(pct, sizeof pct, "%3d%%", static_cast<int>(ratio * 100));
    std::string head = bar(ratio, kBarWidth) + " " + pct + " " + format_bytes(size) + "  ";
    out << head << fit(path, static_cast<size_t>(std::max(10, width_ - kBarWidth - 24))) << "\n";
  }
  if (active > static_cast<size_t>(shown))
    out << "… and " << active - static_cast<size_t>(shown) << " more\n";
  return out.str();
}

}  // namespace eosmirror
