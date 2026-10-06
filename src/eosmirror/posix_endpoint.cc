// SPDX-License-Identifier: GPL-3.0-or-later
#include "eosmirror/posix_endpoint.hh"

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <random>

namespace eosmirror {

namespace {

constexpr std::string_view kTempMarker = ".eosmirror-";

Entry entry_from_stat(std::string name, const struct stat& st) {
  Entry e;
  e.name = std::move(name);
  switch (st.st_mode & S_IFMT) {
    case S_IFREG: e.type = EntryType::File; break;
    case S_IFDIR: e.type = EntryType::Directory; break;
    case S_IFLNK: e.type = EntryType::Symlink; break;
    default: e.type = EntryType::Other; break;
  }
  if (e.type == EntryType::File) e.size = static_cast<uint64_t>(st.st_size);
  e.mtime = {static_cast<int64_t>(st.st_mtim.tv_sec), static_cast<int32_t>(st.st_mtim.tv_nsec)};
  e.uid = st.st_uid;
  e.gid = st.st_gid;
  e.mode = st.st_mode & 07777;
  return e;
}

Result<std::string> read_link(int dirfd, const char* name, const std::string& context) {
  std::string target(256, '\0');
  for (;;) {
    ssize_t n = readlinkat(dirfd, name, target.data(), target.size());
    if (n < 0) return errno_error(errno, "readlink " + context);
    if (static_cast<size_t>(n) < target.size()) {
      target.resize(static_cast<size_t>(n));
      return target;
    }
    target.resize(target.size() * 2);
  }
}

std::string random_suffix() {
  thread_local std::mt19937_64 rng{std::random_device{}()};
  char buf[17];
  std::snprintf(buf, sizeof buf, "%012llx", static_cast<unsigned long long>(rng() & 0xffffffffffffULL));
  return buf;
}

std::string parent_of(const std::string& path) {
  auto slash = path.rfind('/');
  return slash == std::string::npos ? "." : path.substr(0, slash);
}

std::string name_of(const std::string& path) {
  auto slash = path.rfind('/');
  return slash == std::string::npos ? path : path.substr(slash + 1);
}

// ".<name>.eosmirror-<random>" next to the final path, with the name cut
// so that the whole fits into NAME_MAX.
std::string temp_path(const std::string& final_path) {
  std::string name = name_of(final_path);
  constexpr size_t kMaxName = 255 - 1 - 11 - 12;
  if (name.size() > kMaxName) name.resize(kMaxName);
  return parent_of(final_path) + "/." + name + std::string(kTempMarker) + random_suffix();
}

Status apply_metadata_fd(int fd, const std::string& context, const Entry& md, MetaFields fields,
                         ModeBits default_mode) {
  if (has(fields, MetaFields::Owner) && fchown(fd, md.uid, md.gid) != 0)
    return errno_error(errno, "chown " + context);
  ModeBits mode = has(fields, MetaFields::Mode) ? md.mode : default_mode;
  if (fchmod(fd, mode) != 0) return errno_error(errno, "chmod " + context);
  if (has(fields, MetaFields::Mtime)) {
    struct timespec times[2] = {{0, UTIME_OMIT}, {md.mtime.sec, md.mtime.nsec}};
    if (futimens(fd, times) != 0) return errno_error(errno, "utimes " + context);
  }
  return {};
}

class PosixReader : public FileReader {
 public:
  PosixReader(int fd, std::string path) : fd_(fd), path_(std::move(path)) {}
  ~PosixReader() override { close(fd_); }

  Result<size_t> read(uint64_t offset, std::span<std::byte> buf) override {
    size_t done = 0;
    while (done < buf.size()) {
      ssize_t n = pread(fd_, buf.data() + done, buf.size() - done,
                        static_cast<off_t>(offset + done));
      if (n < 0) {
        if (errno == EINTR) continue;
        return errno_error(errno, "read " + path_);
      }
      if (n == 0) break;
      done += static_cast<size_t>(n);
    }
    return done;
  }

  Result<Entry> stat() override {
    struct stat st{};
    if (fstat(fd_, &st) != 0) return errno_error(errno, "stat " + path_);
    return entry_from_stat(name_of(path_), st);
  }

 private:
  int fd_;
  std::string path_;
};

class PosixWriter : public FileWriter {
 public:
  PosixWriter(int fd, std::string temp, std::string final_path, PosixOptions options)
      : fd_(fd), temp_(std::move(temp)), final_(std::move(final_path)), options_(options) {}

  ~PosixWriter() override {
    if (fd_ >= 0) abort();
  }

  Status write(uint64_t offset, std::span<const std::byte> data) override {
    if (offset != written_)
      return Error{ErrorKind::Other, "non-sequential write to " + temp_};
    size_t done = 0;
    while (done < data.size()) {
      ssize_t n = pwrite(fd_, data.data() + done, data.size() - done,
                         static_cast<off_t>(offset + done));
      if (n < 0) {
        if (errno == EINTR) continue;
        return errno_error(errno, "write " + temp_);
      }
      done += static_cast<size_t>(n);
    }
    written_ += data.size();
    return {};
  }

  Status commit(const CommitSpec& spec) override {
    Status status = finish(spec);
    if (!status.ok()) abort();
    return status;
  }

  void abort() override {
    if (fd_ < 0) return;
    close(fd_);
    fd_ = -1;
    unlink(temp_.c_str());
  }

 private:
  Status finish(const CommitSpec& spec) {
    if (written_ != spec.size)
      return Error{ErrorKind::Changed, "wrote " + std::to_string(written_) + " bytes to " + temp_ +
                                           ", expected " + std::to_string(spec.size)};
    if (options_.verify_readback && spec.checksum.type != ChecksumType::None) {
      Status verified = verify_readback(spec.checksum);
      if (!verified.ok()) return verified;
    }
    if (options_.fsync && fsync(fd_) != 0) return errno_error(errno, "fsync " + temp_);
    Status md = apply_metadata_fd(fd_, temp_, spec.metadata, spec.fields, options_.default_mode);
    if (!md.ok()) return md;
    if (close(fd_) != 0) {
      fd_ = -1;
      unlink(temp_.c_str());
      return errno_error(errno, "close " + temp_);
    }
    fd_ = -1;
    if (rename(temp_.c_str(), final_.c_str()) != 0) {
      int err = errno;
      unlink(temp_.c_str());
      return errno_error(err, "rename " + temp_ + " to " + final_);
    }
    return {};
  }

  Status verify_readback(const Checksum& expected) {
    Hasher hasher(expected.type);
    std::vector<std::byte> buf(1 << 20);
    uint64_t offset = 0;
    while (offset < written_) {
      ssize_t n = pread(fd_, buf.data(), buf.size(), static_cast<off_t>(offset));
      if (n < 0) {
        if (errno == EINTR) continue;
        return errno_error(errno, "read back " + temp_);
      }
      if (n == 0) break;
      hasher.update(std::span(buf).first(static_cast<size_t>(n)));
      offset += static_cast<uint64_t>(n);
    }
    Checksum actual = hasher.finish();
    if (offset != written_ || actual != expected)
      return Error{ErrorKind::Checksum, "read back " + temp_ + ": " + actual.hex +
                                            " instead of " + expected.hex};
    return {};
  }

  int fd_;
  std::string temp_;
  std::string final_;
  PosixOptions options_;
  uint64_t written_ = 0;
};

}  // namespace

PosixEndpoint::PosixEndpoint(std::string root, PosixOptions options)
    : root_(std::move(root)), options_(options) {
  while (root_.size() > 1 && root_.back() == '/') root_.pop_back();
  // The canonical path, so that journals recognize the tree however it was
  // named; a root that does not exist yet is made absolute only.
  if (char* real = realpath(root_.c_str(), nullptr)) {
    root_ = real;
    free(real);
  } else if (!root_.empty() && root_[0] != '/') {
    if (char* cwd = getcwd(nullptr, 0)) {
      root_ = std::string(cwd) + "/" + root_;
      free(cwd);
    }
  }
  mode_t mask = umask(0);
  umask(mask);
  options_.default_mode = 0666 & ~static_cast<ModeBits>(mask);
}

Capabilities PosixEndpoint::capabilities() const {
  Capabilities caps;
  caps.mtime_resolution = mtime_resolution();
  caps.can_set_owner = geteuid() == 0;
  caps.can_set_mode = true;
  caps.checksum = ChecksumType::None;
  return caps;
}

// Finds out how precisely the file system under the root stores mtimes, by
// writing a temporary file with a known mtime and reading it back. A root
// that cannot be written is assumed to keep nanoseconds.
int32_t PosixEndpoint::mtime_resolution() const {
  std::call_once(probe_once_, [&] {
    probed_resolution_ = 1;
    // Creating the probe changes the root's mtime, which is restored after.
    struct stat root_st{};
    bool have_root = ::stat(root_.c_str(), &root_st) == 0;
    std::string probe = temp_path(root_ + "/probe");
    int fd = open(probe.c_str(), O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (fd < 0) return;
    constexpr int32_t kNsec = 123456789;
    struct timespec times[2] = {{0, UTIME_OMIT}, {1500000000, kNsec}};
    struct stat st{};
    if (futimens(fd, times) == 0 && fstat(fd, &st) == 0 && st.st_mtim.tv_sec == 1500000000) {
      // The stored value is the probe truncated to the resolution.
      auto stored = static_cast<int32_t>(st.st_mtim.tv_nsec);
      int32_t resolution = 1;
      while (resolution < 1000000000 && kNsec / resolution * resolution != stored) resolution *= 10;
      probed_resolution_ = resolution;
    }
    close(fd);
    unlink(probe.c_str());
    if (have_root) {
      struct timespec restore[2] = {{0, UTIME_OMIT}, root_st.st_mtim};
      utimensat(AT_FDCWD, root_.c_str(), restore, 0);
    }
  });
  return probed_resolution_;
}

bool PosixEndpoint::is_temporary(std::string_view name) const {
  return name.size() > 1 && name[0] == '.' && name.find(kTempMarker) != std::string_view::npos;
}

std::string PosixEndpoint::absolute(const RelPath& path) const {
  if (path.empty()) return root_;
  return root_ == "/" ? "/" + path : root_ + "/" + path;
}

Result<Entry> PosixEndpoint::stat(const RelPath& path) {
  std::string abs = absolute(path);
  struct stat st{};
  // The root itself may be a symlink to the tree.
  int rc = path.empty() ? ::stat(abs.c_str(), &st) : lstat(abs.c_str(), &st);
  if (rc != 0) return errno_error(errno, "stat " + abs);
  Entry e = entry_from_stat(path.empty() ? "" : name_of(path), st);
  if (e.type == EntryType::Symlink) {
    Result<std::string> target = read_link(AT_FDCWD, abs.c_str(), abs);
    if (!target.ok()) return target.error();
    e.link_target = std::move(target).value();
  }
  return e;
}

Result<std::vector<Entry>> PosixEndpoint::list(const RelPath& dir) {
  std::string abs = absolute(dir);
  DIR* d = opendir(abs.c_str());
  if (!d) return errno_error(errno, "open directory " + abs);
  int dirfd = ::dirfd(d);
  std::vector<Entry> entries;
  for (;;) {
    errno = 0;
    struct dirent* ent = readdir(d);
    if (!ent) {
      if (errno != 0) {
        Error err = errno_error(errno, "read directory " + abs);
        closedir(d);
        return err;
      }
      break;
    }
    std::string_view name(ent->d_name);
    if (name == "." || name == "..") continue;
    struct stat st{};
    if (fstatat(dirfd, ent->d_name, &st, AT_SYMLINK_NOFOLLOW) != 0) {
      if (errno == ENOENT) continue;  // vanished since readdir
      Error err = errno_error(errno, "stat " + abs + "/" + std::string(name));
      closedir(d);
      return err;
    }
    Entry e = entry_from_stat(std::string(name), st);
    if (e.type == EntryType::Symlink) {
      Result<std::string> target = read_link(dirfd, ent->d_name, abs + "/" + std::string(name));
      if (!target.ok()) {
        if (target.error().kind == ErrorKind::NotFound) continue;
        closedir(d);
        return target.error();
      }
      e.link_target = std::move(target).value();
    }
    entries.push_back(std::move(e));
  }
  closedir(d);
  return entries;
}

Status PosixEndpoint::mkdir(const RelPath& path, ModeBits mode) {
  std::string abs = absolute(path);
  if (::mkdir(abs.c_str(), mode | S_IRWXU) != 0) return errno_error(errno, "mkdir " + abs);
  return {};
}

Status PosixEndpoint::symlink(const RelPath& path, const std::string& target) {
  std::string abs = absolute(path);
  for (int attempt = 0; attempt < 16; ++attempt) {
    std::string tmp = temp_path(abs);
    if (::symlink(target.c_str(), tmp.c_str()) != 0) {
      if (errno == EEXIST) continue;
      return errno_error(errno, "symlink " + tmp);
    }
    if (rename(tmp.c_str(), abs.c_str()) != 0) {
      int err = errno;
      unlink(tmp.c_str());
      return errno_error(err, "rename " + tmp + " to " + abs);
    }
    return {};
  }
  return Error{ErrorKind::Other, "no free temporary name for " + abs};
}

Status PosixEndpoint::set_metadata(const RelPath& path, const Entry& md, MetaFields fields) {
  std::string abs = absolute(path);
  if (has(fields, MetaFields::Owner) && lchown(abs.c_str(), md.uid, md.gid) != 0)
    return errno_error(errno, "chown " + abs);
  if (has(fields, MetaFields::Mode) && md.type != EntryType::Symlink &&
      fchmodat(AT_FDCWD, abs.c_str(), md.mode, 0) != 0)
    return errno_error(errno, "chmod " + abs);
  if (has(fields, MetaFields::Mtime)) {
    struct timespec times[2] = {{0, UTIME_OMIT}, {md.mtime.sec, md.mtime.nsec}};
    if (utimensat(AT_FDCWD, abs.c_str(), times, AT_SYMLINK_NOFOLLOW) != 0)
      return errno_error(errno, "utimes " + abs);
  }
  return {};
}

Status PosixEndpoint::remove(const RelPath& path, EntryType type) {
  std::string abs = absolute(path);
  int rc = type == EntryType::Directory ? rmdir(abs.c_str()) : unlink(abs.c_str());
  if (rc != 0) return errno_error(errno, "remove " + abs);
  return {};
}

Result<std::unique_ptr<FileReader>> PosixEndpoint::open_read(const RelPath& path) {
  std::string abs = absolute(path);
  // O_NONBLOCK, so that a FIFO that replaced the file cannot block the open.
  int fd = open(abs.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
  if (fd < 0) return errno_error(errno, "open " + abs);
  struct stat st{};
  if (fstat(fd, &st) != 0) {
    int err = errno;
    close(fd);
    return errno_error(err, "stat " + abs);
  }
  if (!S_ISREG(st.st_mode)) {
    close(fd);
    return Error{ErrorKind::Changed, abs + " is not a regular file"};
  }
  fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) & ~O_NONBLOCK);
  return std::unique_ptr<FileReader>(new PosixReader(fd, std::move(abs)));
}

Result<std::unique_ptr<FileWriter>> PosixEndpoint::open_write(const RelPath& path,
                                                              const CommitSpec& /*spec*/) {
  std::string abs = absolute(path);
  for (int attempt = 0; attempt < 16; ++attempt) {
    std::string tmp = temp_path(abs);
    int fd = open(tmp.c_str(), O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (fd < 0) {
      if (errno == EEXIST) continue;
      return errno_error(errno, "create " + tmp);
    }
    return std::unique_ptr<FileWriter>(new PosixWriter(fd, std::move(tmp), std::move(abs), options_));
  }
  return Error{ErrorKind::Other, "no free temporary name for " + abs};
}

}  // namespace eosmirror
