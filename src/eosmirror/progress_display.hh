// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <atomic>
#include <string>
#include <thread>

#include "eosmirror/report.hh"

namespace eosmirror {

// A live progress display on a terminal: counters and rates, bars for the
// transfer slots in use and the backlog, and one bar per running transfer.
// It redraws in place a few times per second and steps aside for log lines.
class ProgressDisplay {
 public:
  ProgressDisplay(const Report& report, std::string title);
  ~ProgressDisplay();

  // Whether a display makes sense on the given stream (a terminal).
  static bool suitable(int fd);

  void start();
  // Removes the display from the terminal and stops redrawing.
  void stop();

  // The display's lines for a terminal of the given width, each narrower,
  // so that it can be redrawn in place.
  std::string lines(int terminal_width) const;

 private:
  void loop();
  void render();
  void erase();

  const Report& report_;
  std::string title_;
  std::thread thread_;
  std::atomic<bool> stop_{false};
  int lines_shown_ = 0;  // under the log mutex
  int width_ = 80;
};

}  // namespace eosmirror
