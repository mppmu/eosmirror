// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <string>
#include <vector>

#include "eosmirror/endpoint.hh"

namespace eosmirror {

struct SelftestCheck {
  std::string name;
  bool ok = true;
  bool skipped = false;  // not applicable to the endpoint
  std::string message;
};

struct SelftestReport {
  std::vector<SelftestCheck> checks;
  bool ok() const;
};

// Exercises everything a run needs on a target before a long run: creating
// a directory and files with verified checksums, setting modes, owners and
// mtimes, symlinks, listings and removal. Works in a temporary directory
// under the target's root, which is removed afterwards.
SelftestReport run_selftest(Endpoint& target, bool with_owner);

}  // namespace eosmirror
