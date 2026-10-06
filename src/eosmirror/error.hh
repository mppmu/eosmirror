// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace eosmirror {

// Journals store kinds as numbers: new kinds go last.
enum class ErrorKind : uint8_t {
  NotFound,
  Exists,
  Permission,
  NotADirectory,
  IsADirectory,
  NotEmpty,
  Unsupported,
  IO,
  Timeout,
  Changed,    // the source changed while it was being copied
  Checksum,   // the data arrived with the wrong checksum
  Cancelled,  // the run is being stopped
  Other,
  NoSpace,  // no space left or quota exceeded
};

std::string_view to_string(ErrorKind kind);

// An error worth retrying after a short wait: the condition is likely to be
// transient (network trouble, timeouts, busy servers, a file changing).
bool is_transient(ErrorKind kind);

struct Error {
  ErrorKind kind = ErrorKind::Other;
  std::string message;
  int errnum = 0;  // the errno, where one is known

  // A message of the form "<message>: <strerror>" when an errno is known.
  std::string describe() const;
};

// Builds an error from an errno value, with the context as message.
Error errno_error(int err, std::string_view context);

// The error of a failed write or close of an upload. The target then holds
// no file, so copying again is right unless space or permission is missing:
// errors of other kinds become transient I/O errors.
Error upload_error(Error e);

// The result of an operation: either a value or an error.
template <class T>
class [[nodiscard]] Result {
 public:
  Result(T value) : value_(std::move(value)) {}  // NOLINT(google-explicit-constructor)
  Result(Error error) : error_(std::move(error)) {}  // NOLINT(google-explicit-constructor)

  bool ok() const { return !error_; }
  explicit operator bool() const { return ok(); }

  T& value() & { return *value_; }
  const T& value() const& { return *value_; }
  T&& value() && { return std::move(*value_); }
  const Error& error() const { return *error_; }

 private:
  std::optional<T> value_;
  std::optional<Error> error_;
};

template <>
class [[nodiscard]] Result<void> {
 public:
  Result() = default;
  Result(Error error) : error_(std::move(error)) {}  // NOLINT(google-explicit-constructor)

  bool ok() const { return !error_; }
  explicit operator bool() const { return ok(); }
  const Error& error() const { return *error_; }

 private:
  std::optional<Error> error_;
};

using Status = Result<void>;

}  // namespace eosmirror
