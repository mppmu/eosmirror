// SPDX-License-Identifier: GPL-3.0-or-later
#include "eosmirror/progress_display.hh"

#include <sys/ioctl.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <sstream>
#include <string_view>
#include <vector>

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

// The text cut at the front to at most width columns, counting bytes, which
// is never fewer than the columns that UTF-8 characters take.
std::string fit(const std::string& text, size_t width) {
  if (text.size() <= width) return text;
  if (width < 4) return text.substr(0, width);
  return "…" + text.substr(text.size() - width + 1);
}

bool continuation_byte(char c) { return (static_cast<unsigned char>(c) & 0xc0) == 0x80; }

// The columns that a line of printable UTF-8 text takes on a terminal, one
// per character.
size_t columns(std::string_view text) {
  return static_cast<size_t>(
      std::count_if(text.begin(), text.end(), [](char c) { return !continuation_byte(c); }));
}

// The line cut at the end to at most width columns.
std::string clip(const std::string& line, size_t width) {
  size_t cols = 0;
  for (size_t i = 0; i < line.size(); ++i) {
    if (continuation_byte(line[i])) continue;
    if (cols++ == width) return line.substr(0, i);
  }
  return line;
}

// The items separated by two spaces, in lines of at most width columns.
std::string flow(const std::vector<std::string>& items, size_t width) {
  std::string out, line;
  for (const std::string& item : items) {
    if (!line.empty() && columns(line) + 2 + columns(item) > width) {
      out += clip(line, width) + "\n";
      line.clear();
    }
    line += (line.empty() ? "" : "  ") + item;
  }
  if (!line.empty()) out += clip(line, width) + "\n";
  return out;
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
  std::string text = lines(width_);
  int count = static_cast<int>(std::count(text.begin(), text.end(), '\n'));
  std::string out;
  std::istringstream in(text);
  std::string line;
  while (std::getline(in, line)) out += "\r\x1b[K" + line + "\n";
  // Blank any lines the previous display had beyond this one.
  for (int i = count; i < lines_shown_; ++i) out += "\r\x1b[K\n";
  int total = std::max(count, lines_shown_);
  if (total > 0) out += "\x1b[" + std::to_string(total) + "A";
  std::fputs(out.c_str(), stderr);
  std::fflush(stderr);
  lines_shown_ = count;
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

std::string ProgressDisplay::lines(int terminal_width) const {
  const Stats& s = report_.stats;
  // A line as wide as the terminal wraps on some.
  const size_t width = static_cast<size_t>(std::max(terminal_width, 21) - 1);
  double secs = std::chrono::duration<double>(report_.elapsed()).count();
  auto count = [](const char* label, uint64_t n) { return label + std::to_string(n); };

  std::string elapsed = format_duration(report_.elapsed());
  std::string out =
      clip(printable(fit(title_, width - std::min(width, elapsed.size() + 2))) + "  " + elapsed,
           width) +
      "\n";

  std::vector<std::string> counters = {
      count("dirs ", s.dirs_listed.load()), count("files ", s.files_checked.load()) + " checked",
      std::to_string(s.files_copied.load()) + " copied (" + format_bytes(s.bytes_copied.load()) +
          ")",
      std::to_string(s.files_unchanged.load()) + " unchanged"};
  if (uint64_t links = s.symlinks_created.load() + s.symlinks_unchanged.load(); links > 0)
    counters.push_back(count("links ", links));
  if (s.metadata_fixed.load() > 0) counters.push_back(count("fixed ", s.metadata_fixed.load()));
  if (s.deleted.load() > 0) counters.push_back(count("deleted ", s.deleted.load()));
  counters.push_back(count("failed ", s.failures.load()));
  if (s.retries.load() > 0) counters.push_back(count("retries ", s.retries.load()));
  out += flow(counters, width);

  size_t active = 0;
  for (const auto& slot : report_.slots)
    if (slot->active.load()) ++active;
  size_t slots = report_.slots.size();
  size_t queued = s.queued_copies.load();
  size_t backlog_max = std::max<size_t>(report_.backlog_capacity, 1);
  size_t limit = std::min<size_t>(s.transfer_limit.load(), slots);
  out += flow({"written " + format_bytes(s.bytes_written.load()) + " at " +
                   rate(s.bytes_written.load(), secs),
               "transfers " +
                   bar(slots ? static_cast<double>(active) / static_cast<double>(slots) : 0,
                       kBarWidth / 2) +
                   " " + std::to_string(active) + "/" + std::to_string(limit) + " of " +
                   std::to_string(slots),
               "backlog " +
                   bar(static_cast<double>(queued) / static_cast<double>(backlog_max),
                       kBarWidth / 2) +
                   " " + std::to_string(queued)},
              width);

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
    // The size and the count may belong to different files for a moment.
    uint64_t written = slot->written.load();
    double ratio =
        size ? std::min(1.0, static_cast<double>(written) / static_cast<double>(size)) : 1.0;
    char pct[8];
    std::snprintf(pct, sizeof pct, "%3d%%", static_cast<int>(ratio * 100));
    std::string head = bar(ratio, kBarWidth) + " " + pct + " " + format_bytes(size) + "  ";
    size_t room = width > columns(head) + 10 ? width - columns(head) : 10;
    out += clip(head + printable(fit(path, room)), width) + "\n";
  }
  if (active > static_cast<size_t>(shown))
    out += clip("… and " + std::to_string(active - static_cast<size_t>(shown)) + " more", width) +
           "\n";
  return out;
}

}  // namespace eosmirror
