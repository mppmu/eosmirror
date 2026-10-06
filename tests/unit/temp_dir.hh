// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <sys/stat.h>
#include <unistd.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

// A fresh directory under $TMPDIR, removed with its contents on destruction.
class TempDir {
 public:
  TempDir() {
    const char* base = std::getenv("TMPDIR");
    std::string tmpl = std::string(base && *base ? base : "/tmp") + "/eosmirror-test-XXXXXX";
    path_ = mkdtemp(tmpl.data());
  }
  ~TempDir() {
    std::error_code ec;
    std::filesystem::remove_all(path_, ec);
  }
  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;

  const std::string& path() const { return path_; }
  std::string sub(const std::string& rel) const { return path_ + "/" + rel; }

  void write_file(const std::string& rel, const std::string& content, mode_t mode = 0644) const {
    std::string p = sub(rel);
    std::filesystem::create_directories(std::filesystem::path(p).parent_path());
    std::ofstream(p, std::ios::binary) << content;
    chmod(p.c_str(), mode);
  }

  std::string read_file(const std::string& rel) const {
    std::ifstream in(sub(rel), std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), {});
  }

 private:
  std::string path_;
};

inline bool is_root() { return geteuid() == 0; }
