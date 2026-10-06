// SPDX-License-Identifier: GPL-3.0-or-later
#include "eosmirror/progress_display.hh"

#include <sstream>

#include "doctest/doctest.h"

using namespace eosmirror;

namespace {

size_t columns(const std::string& line) {
  size_t n = 0;
  for (char c : line) n += (static_cast<unsigned char>(c) & 0xc0) != 0x80;
  return n;
}

}  // namespace

TEST_CASE("every line of the progress display fits the terminal") {
  Report report;
  report.init_slots(3, 10000);
  Stats& s = report.stats;
  for (Counter* c : {&s.dirs_listed, &s.files_checked, &s.files_copied, &s.files_unchanged,
                     &s.symlinks_created, &s.metadata_fixed, &s.deleted, &s.failures, &s.retries})
    c->store(123456789);
  s.bytes_copied = 1ull << 50;
  s.bytes_written = 1ull << 50;
  s.transfer_limit = 3;
  s.queued_copies = 9999;
  // A retried copy has written more than the file's size, and another slot
  // already shows a new file next to the old one's count.
  const std::string long_path = std::string(300, 'p') + "/Größe/日本/" + std::string(50, 'q');
  for (auto& slot : report.slots) {
    slot->path = long_path;
    slot->active = true;
  }
  report.slots[0]->size = 1000;
  report.slots[0]->written = 3000;
  report.slots[1]->size = 1;
  report.slots[1]->written = 1ull << 40;
  report.slots[2]->size = 0;

  ProgressDisplay display(report, "eosmirror " + long_path + " -> " + long_path);
  for (int width : {200, 80, 40, 21}) {
    INFO("width ", width);
    std::istringstream text(display.lines(width));
    int lines = 0;
    for (std::string line; std::getline(text, line); ++lines) {
      INFO(line);
      CHECK(columns(line) < static_cast<size_t>(width));
      CHECK(line.find("300%") == std::string::npos);
    }
    CHECK(lines >= 6);  // title, counters, transfers and one line per slot
  }
  std::string wide = display.lines(200);
  CHECK(wide.find("100%") != std::string::npos);
  CHECK(wide.find("failed 123456789") != std::string::npos);
}
