// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <string>
#include <vector>

#include "eosmirror/endpoints.hh"
#include "eosmirror/error.hh"
#include "eosmirror/log.hh"
#include "eosmirror/options.hh"

namespace eosmirror {

enum class Command { Sync, Failures, Help, Version };

struct CliOptions {
  Command command = Command::Help;
  std::string source;
  std::string target;
  SyncOptions sync;
  EndpointSettings endpoints;
  std::string journal;
  bool retry_failed = false;
  int progress_seconds = 0;
  LogLevel log_level = LogLevel::Info;
};

// Parses the command line. A usage error is returned as an Error whose
// message is meant for the user.
Result<CliOptions> parse_command_line(int argc, char** argv);

std::string usage();

// Parses sizes like "64k", "8M", "1G" or plain bytes.
Result<uint64_t> parse_size(const std::string& text);

}  // namespace eosmirror
