// SPDX-License-Identifier: GPL-3.0-or-later
#include "eosmirror/posix_endpoint.hh"

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/vfs.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <mutex>
#include <optional>
#include <system_error>
#include <thread>
#include <vector>

#include "eosmirror/log.hh"

namespace eosmirror {

// Copies renamed into place with mtime 0, which flush() gives their mtimes
// once a sync of their file system has followed the rename. Adding a copy
// never waits for a sync.
class DeferredMtimes {
 public:
  ~DeferredMtimes() {
    if (Status s = flush(); !s.ok()) log::warn(s.error().describe());
  }

  // A copy renamed into place at path, the file ino on device dev.
  void add(std::string path, Timespec mtime, dev_t dev, ino_t ino) {
    std::lock_guard lock(mutex_);
    pending_.push_back({std::move(path), mtime, dev, ino});
  }

  // Syncs the file systems of the copies added so far, each through the
  // directory of one of them, and gives those that were synced their mtimes.
  // Copies whose sync or mtime failed keep mtime 0, so that the next run
  // copies them again. A file that has replaced a copy since (another run's
  // copy, or a user's file) is left alone.
  Status flush() {
    std::lock_guard flush_lock(flush_mutex_);
    std::vector<Copy> batch;
    {
      std::lock_guard lock(mutex_);
      batch.swap(pending_);
    }
    std::map<dev_t, bool> synced;
    Outcome outcome;
    for (const Copy& c : batch) {
      auto [it, added] = synced.emplace(c.dev, false);
      if (!added) continue;
      std::string dir = c.path.substr(0, c.path.rfind('/'));
      int fd = open(dir.empty() ? "/" : dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
      it->second = fd >= 0 && syncfs(fd) == 0;
      if (!it->second && !outcome.first)
        outcome.first = errno_error(errno, "sync the file system of " + dir);
      if (fd >= 0) close(fd);
    }
    std::erase_if(batch, [&](const Copy& c) {
      if (synced[c.dev]) return false;
      ++outcome.failed;
      return true;
    });
    // Many copies get their mtimes in several threads, since a run waits for
    // its last flush. This thread takes over the shares of threads that
    // cannot be started.
    size_t workers = std::min<size_t>(8, (batch.size() + 999) / 1000);
    std::vector<Outcome> outcomes(workers);
    std::vector<std::thread> threads;
    size_t started = 1;
    try {
      for (; started < workers; ++started)
        threads.emplace_back([&, w = started] { apply(batch, w, workers, outcomes[w]); });
    } catch (const std::system_error& e) {
      log::warn("giving copies their mtimes in fewer threads: ", e.what());
    }
    if (workers > 0) apply(batch, 0, workers, outcomes[0]);
    for (size_t w = started; w < workers; ++w) apply(batch, w, workers, outcomes[w]);
    for (auto& t : threads) t.join();
    for (const Outcome& o : outcomes) {
      if (!outcome.first) outcome.first = o.first;
      outcome.failed += o.failed;
    }
    if (!outcome.first) return {};
    Error e = *outcome.first;
    e.message = e.describe() + ": " + std::to_string(outcome.failed) + " copies keep mtime 0";
    e.errnum = 0;
    return e;
  }

 private:
  struct Copy {
    std::string path;
    Timespec mtime;
    dev_t dev;
    ino_t ino;
  };
  // The outcome of giving mtimes: the first error, and the number of copies
  // that keep mtime 0.
  struct Outcome {
    std::optional<Error> first;
    size_t failed = 0;
  };

  // Gives every n-th copy from the k-th on its mtime.
  static void apply(const std::vector<Copy>& batch, size_t k, size_t n, Outcome& outcome) {
    for (size_t i = k; i < batch.size(); i += n) {
      const Copy& c = batch[i];
      // Through a descriptor of the file, so that the mtime cannot go to a
      // file that replaces the copy between its identification and that.
      int fd = open(c.path.c_str(), O_PATH | O_NOFOLLOW | O_CLOEXEC);
      if (fd < 0) {
        if (errno != ENOENT) {
          if (!outcome.first) outcome.first = errno_error(errno, "open " + c.path);
          ++outcome.failed;
        }
        continue;
      }
      struct stat st{};
      bool ours = fstat(fd, &st) == 0 && st.st_dev == c.dev && st.st_ino == c.ino;
      struct timespec times[2] = {{0, UTIME_OMIT}, {c.mtime.sec, c.mtime.nsec}};
      // The empty path names the descriptor's file (futimens() takes no
      // O_PATH descriptor).
      bool set = ours && utimensat(fd, "", times, AT_EMPTY_PATH) == 0;
      int err = errno;
      close(fd);
      if (set) continue;
      if (ours && !outcome.first) outcome.first = errno_error(err, "utimes " + c.path);
      ++outcome.failed;  // replaced, or not given its mtime
    }
  }

  std::mutex mutex_;  // for pending_
  std::vector<Copy> pending_;
  std::mutex flush_mutex_;  // one flush at a time
};

namespace {

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
  if (e.type == EntryType::Directory)
    e.id = std::to_string(st.st_dev) + ":" + std::to_string(st.st_ino);
  e.mtime = {static_cast<int64_t>(st.st_mtim.tv_sec), static_cast<int32_t>(st.st_mtim.tv_nsec)};
  e.ctime = {static_cast<int64_t>(st.st_ctim.tv_sec), static_cast<int32_t>(st.st_ctim.tv_nsec)};
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

// The device in a directory's id.
std::string_view device_of(std::string_view id) { return id.substr(0, id.find(':')); }

std::string parent_of(const std::string& path) {
  auto slash = path.rfind('/');
  return slash == std::string::npos ? "." : path.substr(0, slash);
}

std::string name_of(const std::string& path) {
  auto slash = path.rfind('/');
  return slash == std::string::npos ? path : path.substr(slash + 1);
}

// A temporary name next to the final path.
std::string temp_path(const std::string& final_path) {
  return parent_of(final_path) + "/" + temporary_name(name_of(final_path));
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
  // deferred: where copies get their mtimes after a sync, if at all.
  PosixWriter(int fd, std::string temp, std::string final_path, PosixOptions options,
              DeferredMtimes* deferred)
      : fd_(fd),
        temp_(std::move(temp)),
        final_(std::move(final_path)),
        options_(options),
        deferred_(deferred) {}

  ~PosixWriter() override {
    if (fd_ >= 0) (void)abort();
  }

  Status write(Chunk chunk) override {
    if (chunk.offset != written_)
      return Error{ErrorKind::Other, "non-sequential write to " + temp_};
    auto data = chunk.data();
    size_t done = 0;
    while (done < data.size()) {
      ssize_t n = pwrite(fd_, data.data() + done, data.size() - done,
                         static_cast<off_t>(chunk.offset + done));
      if (n < 0) {
        if (errno == EINTR) continue;
        return errno_error(errno, "write " + temp_);
      }
      done += static_cast<size_t>(n);
    }
    written_ += data.size();
    return {};
  }

  Result<CommitInfo> commit(const CommitSpec& spec) override {
    Status status = finish(spec);
    if (!status.ok()) {
      (void)abort();
      return status.error();
    }
    return CommitInfo{verified_};
  }

  Status abort() override {
    if (fd_ < 0) return {};
    close(fd_);
    fd_ = -1;
    unlink(temp_.c_str());
    return {};
  }

 private:
  Status finish(const CommitSpec& spec) {
    if (written_ != spec.size)
      return Error{ErrorKind::Changed, "wrote " + std::to_string(written_) + " bytes to " + temp_ +
                                           ", expected " + std::to_string(spec.size)};
    // The writeback starts now, so that the later sync only has to finish it.
    if (deferred_) sync_file_range(fd_, 0, 0, SYNC_FILE_RANGE_WRITE);
    // Reading the file back is the only verification a local target has.
    if ((options_.verify_readback || spec.require_verification) &&
        spec.checksum.type != ChecksumType::None) {
      Status verified = verify_readback(spec.checksum);
      if (!verified.ok()) return verified;
      verified_ = true;
    } else if (spec.require_verification) {
      return Error{ErrorKind::Unsupported, "no checksum to verify " + temp_ + " against"};
    }
    // Until a sync has made it durable, the copy has mtime 0, so that the
    // next run copies it again if this one ends before the sync. A source
    // file with mtime 0 itself could not be told apart from such a copy, so
    // its copy is made durable at once.
    bool deferred = deferred_ && has(spec.fields, MetaFields::Mtime) && spec.metadata.mtime.sec != 0;
    if (options_.sync && !deferred && fsync(fd_) != 0) return errno_error(errno, "fsync " + temp_);
    Entry metadata = spec.metadata;
    if (deferred) metadata.mtime = {0, 0};
    Status md = apply_metadata_fd(fd_, temp_, metadata, spec.fields, options_.default_mode);
    if (!md.ok()) return md;
    struct stat st{};
    if (deferred && fstat(fd_, &st) != 0) return errno_error(errno, "stat " + temp_);
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
    if (deferred) {
      deferred_->add(final_, spec.metadata.mtime, st.st_dev, st.st_ino);
    } else if (options_.sync) {
      // No sync of the file system need follow before the directory of a
      // copy made durable at once is recorded as done: its rename is synced
      // here.
      std::string dir = final_.substr(0, final_.rfind('/'));
      int dfd = open(dir.empty() ? "/" : dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
      bool synced = dfd >= 0 && fsync(dfd) == 0;
      int err = errno;
      if (dfd >= 0) close(dfd);
      if (!synced) return errno_error(err, "sync the directory of " + final_);
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
  DeferredMtimes* deferred_;
  uint64_t written_ = 0;
  bool verified_ = false;
};

}  // namespace

Result<std::string> canonical_path(const std::string& path) {
  std::error_code ec;
  std::filesystem::path absolute = std::filesystem::absolute(path, ec);
  if (ec) return errno_error(ec.value(), "resolve the path " + path);
  std::filesystem::path canonical = std::filesystem::weakly_canonical(absolute, ec);
  for (int i = 0; !ec && i < 40; ++i) {
    std::filesystem::path again = std::filesystem::weakly_canonical(canonical, ec);
    if (ec || again == canonical) break;
    canonical = again;
  }
  if (ec) return errno_error(ec.value(), "resolve the path " + path);
  return canonical.string();
}

PosixEndpoint::PosixEndpoint(std::string root, PosixOptions options)
    : root_(std::move(root)), options_(options) {
  // The canonical path, so that journals recognize the tree however it was
  // named, and so that the overlap check sees where the tree really is. A
  // root that does not exist yet gets the canonical path of its deepest
  // existing ancestor, which is what it will have once created. One that
  // cannot be resolved (a directory above it that cannot be searched) is
  // kept as given, and root_error() says so: such a path could name a tree
  // inside the other endpoint's without the overlap check seeing it.
  auto canonical = canonical_path(root_);
  if (canonical.ok())
    root_ = canonical.value();
  else
    root_error_ = canonical.error();
  while (root_.size() > 1 && root_.back() == '/') root_.pop_back();
  mode_t mask = umask(0);
  umask(mask);
  options_.default_mode = 0666 & ~static_cast<ModeBits>(mask);
  if (options_.sync) deferred_ = std::make_unique<DeferredMtimes>();
}

PosixEndpoint::~PosixEndpoint() = default;

Status PosixEndpoint::flush() { return deferred_ ? deferred_->flush() : Status(); }

Capabilities PosixEndpoint::capabilities() const {
  Capabilities caps;
  caps.mtime_resolution = mtime_resolution_.load();
  caps.can_set_owner = geteuid() == 0;
  caps.can_set_mode = true;
  caps.checksum = ChecksumType::None;
  return caps;
}

// Finds out how precisely the file system under the root stores mtimes, by
// writing a temporary file with a known mtime and reading it back. A root
// that cannot be written is assumed to keep nanoseconds.
void PosixEndpoint::probe_target() {
  std::call_once(probe_once_, [&] {
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
      mtime_resolution_ = resolution;
    }
    close(fd);
    unlink(probe.c_str());
    if (have_root) {
      struct timespec restore[2] = {{0, UTIME_OMIT}, root_st.st_mtim};
      utimensat(AT_FDCWD, root_.c_str(), restore, 0);
    }
  });
}

// Files are written, and symlinks made, under temporary names.
bool PosixEndpoint::is_temporary(const Entry& entry) const {
  return (entry.type == EntryType::File || entry.type == EntryType::Symlink) &&
         is_temporary_name(entry.name);
}

bool PosixEndpoint::other_file_system(const Entry& dir) const {
  std::call_once(device_once_, [&] {
    struct stat st{};
    if (::stat(root_.c_str(), &st) == 0) device_ = std::to_string(st.st_dev);
  });
  return dir.type == EntryType::Directory && !device_.empty() && !dir.id.empty() &&
         device_of(dir.id) != device_;
}

std::optional<std::string> mount_type(std::string_view mountinfo, std::string_view path) {
  // "36 35 98:0 /root /mount/point rw,noatime [optional fields] - type source options",
  // with spaces, tabs, line breaks and backslashes in the mount point as \ooo.
  auto unescape = [](std::string_view s) {
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
      if (s[i] == '\\' && i + 3 < s.size()) {
        out += static_cast<char>(std::strtol(std::string(s.substr(i + 1, 3)).c_str(), nullptr, 8));
        i += 3;
      } else {
        out += s[i];
      }
    }
    return out;
  };
  std::optional<std::string> type;
  size_t best = 0;
  while (!mountinfo.empty()) {
    std::string_view line = mountinfo.substr(0, mountinfo.find('\n'));
    mountinfo.remove_prefix(std::min(mountinfo.size(), line.size() + 1));
    std::vector<std::string_view> fields;
    for (size_t i = 0; i < line.size();) {
      size_t space = std::min(line.find(' ', i), line.size());
      fields.push_back(line.substr(i, space - i));
      i = space + 1;
    }
    auto separator = std::find(fields.begin() + std::min<size_t>(fields.size(), 6), fields.end(), "-");
    if (fields.size() < 5 || separator == fields.end() || separator + 1 == fields.end()) continue;
    std::string point = unescape(fields[4]);
    bool above = point == "/" || path == point ||
                 (path.starts_with(point) && path.size() > point.size() && path[point.size()] == '/');
    if (!above || point.size() < best) continue;
    best = point.size();
    type = std::string(*(separator + 1));
  }
  return type;
}

// Writing to EOS through eosxd breaks erasure-coded files: up to EOS 5.5.2,
// stripes written out of order get zero parity.
std::string PosixEndpoint::target_warning() const {
  constexpr std::string_view kAdvice =
      "writing EOS through a FUSE mount is slow and unsafe for erasure-coded directories; use "
      "root://... or /eos/... instead";
  std::ifstream in("/proc/self/mountinfo");
  std::string table{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
  std::optional<std::string> type = mount_type(table, root_);
  if (type == "fuse.eosxd") return root_ + " is on an EOS FUSE mount: " + std::string(kAdvice);
  if (!type) {
    // Without a mount table, any FUSE file system may be one.
    constexpr long kFuseSuperMagic = 0x65735546;  // FUSE_SUPER_MAGIC
    std::string path = root_;
    struct statfs fs{};
    while (statfs(path.c_str(), &fs) != 0) {
      if (path.empty() || path == "/") return {};
      path = parent_of(path);
      if (path.empty()) path = "/";
    }
    if (fs.f_type == kFuseSuperMagic) type = "fuse";
  }
  if (type != "fuse") return {};
  return root_ + " is on a FUSE file system; if that is an EOS FUSE mount, " +
         std::string(kAdvice);
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

Result<Listing> PosixEndpoint::list(const RelPath& dir) {
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
  return Listing{std::move(entries)};
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
  // The mtime first, then the owner, then the mode: the mode may take away
  // write access, and chown clears setuid bits.
  if (has(fields, MetaFields::Mtime)) {
    struct timespec times[2] = {{0, UTIME_OMIT}, {md.mtime.sec, md.mtime.nsec}};
    if (utimensat(AT_FDCWD, abs.c_str(), times, AT_SYMLINK_NOFOLLOW) != 0)
      return errno_error(errno, "utimes " + abs);
  }
  if (has(fields, MetaFields::Owner) && lchown(abs.c_str(), md.uid, md.gid) != 0)
    return errno_error(errno, "chown " + abs);
  if (has(fields, MetaFields::Mode) && md.type != EntryType::Symlink &&
      fchmodat(AT_FDCWD, abs.c_str(), md.mode, 0) != 0)
    return errno_error(errno, "chmod " + abs);
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
    return std::unique_ptr<FileWriter>(
        new PosixWriter(fd, std::move(tmp), std::move(abs), options_, deferred_.get()));
  }
  return Error{ErrorKind::Other, "no free temporary name for " + abs};
}

}  // namespace eosmirror
