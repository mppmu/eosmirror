// SPDX-License-Identifier: GPL-3.0-or-later
#include "eosmirror/error.hh"

#include <cerrno>
#include <cstring>

namespace eosmirror {

std::string_view to_string(ErrorKind kind) {
  switch (kind) {
    case ErrorKind::NotFound: return "not found";
    case ErrorKind::Exists: return "exists";
    case ErrorKind::Permission: return "permission denied";
    case ErrorKind::NotADirectory: return "not a directory";
    case ErrorKind::IsADirectory: return "is a directory";
    case ErrorKind::NotEmpty: return "directory not empty";
    case ErrorKind::Unsupported: return "unsupported";
    case ErrorKind::IO: return "I/O error";
    case ErrorKind::Timeout: return "timeout";
    case ErrorKind::Changed: return "changed during copy";
    case ErrorKind::Checksum: return "checksum mismatch";
    case ErrorKind::Cancelled: return "cancelled";
    case ErrorKind::Other: return "error";
  }
  return "error";
}

bool is_transient(ErrorKind kind) {
  switch (kind) {
    case ErrorKind::IO:
    case ErrorKind::Timeout:
    case ErrorKind::Changed:
    case ErrorKind::Checksum:
      return true;
    default:
      return false;
  }
}

std::string Error::describe() const {
  if (errnum == 0) return message;
  return message + ": " + std::strerror(errnum);
}

Error errno_error(int err, std::string_view context) {
  ErrorKind kind;
  switch (err) {
    case ENOENT: kind = ErrorKind::NotFound; break;
    case EEXIST: kind = ErrorKind::Exists; break;
    case EACCES:
    case EPERM: kind = ErrorKind::Permission; break;
    case ENOTDIR: kind = ErrorKind::NotADirectory; break;
    case EISDIR: kind = ErrorKind::IsADirectory; break;
    case ENOTEMPTY: kind = ErrorKind::NotEmpty; break;
    case ENOSYS:
    case EOPNOTSUPP: kind = ErrorKind::Unsupported; break;
    case ETIMEDOUT: kind = ErrorKind::Timeout; break;
    case EIO:
    case EAGAIN:
    case EINTR:
    case ESTALE:
    case EBUSY:
    case ENOMEM:
    case ENOSPC:
    case EDQUOT:
    case ECONNRESET:
    case ECONNREFUSED:
    case EHOSTUNREACH:
    case ENETUNREACH: kind = ErrorKind::IO; break;
    default: kind = ErrorKind::Other; break;
  }
  return Error{kind, std::string(context), err};
}

}  // namespace eosmirror
