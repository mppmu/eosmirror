// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <unistd.h>

#include <cstdio>
#include <string>

// Redirects stderr into a temporary file while it lives; text() returns
// what was written and ends the capture.
class StderrCapture {
 public:
  StderrCapture() : file_(std::tmpfile()) {
    std::fflush(stderr);
    saved_ = dup(STDERR_FILENO);
    dup2(fileno(file_), STDERR_FILENO);
  }
  ~StderrCapture() {
    restore();
    std::fclose(file_);
  }
  StderrCapture(const StderrCapture&) = delete;
  StderrCapture& operator=(const StderrCapture&) = delete;

  std::string text() {
    restore();
    std::rewind(file_);
    std::string out;
    char buf[4096];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, file_)) > 0) out.append(buf, n);
    return out;
  }

 private:
  void restore() {
    if (saved_ < 0) return;
    std::fflush(stderr);
    dup2(saved_, STDERR_FILENO);
    close(saved_);
    saved_ = -1;
  }

  std::FILE* file_;
  int saved_ = -1;
};
