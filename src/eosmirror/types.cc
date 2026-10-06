// SPDX-License-Identifier: GPL-3.0-or-later
#include "eosmirror/types.hh"

namespace eosmirror {

std::string_view to_string(EntryType type) {
  switch (type) {
    case EntryType::File: return "file";
    case EntryType::Directory: return "directory";
    case EntryType::Symlink: return "symlink";
    case EntryType::Other: return "special file";
  }
  return "entry";
}

}  // namespace eosmirror
