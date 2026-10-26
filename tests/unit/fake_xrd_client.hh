// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <XProtocol/XProtocol.hh>
#include <XrdCl/XrdClXRootDResponses.hh>

#include <cctype>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "eosmirror/checksum.hh"
#include "eosmirror/xrd_client.hh"

// An in-memory EOS instance behind the XrdClient interface, for tests of the
// XRootD and EOS endpoints without a server.
//
// It answers what the endpoints send the way an MGM and its storage nodes do
// (see docs/design.md): MGM commands opened under /proc/user/, fsctl queries
// (stat, readlink, getfmd, utimes, chmod, chown), atomic uploads that are
// kept under a hidden name until their close renames them into place, and
// files opened for reading with the id of the file in the redirection URL.
// It answers plain XRootD requests as well (stat, dirlist, mkdir, rm, rmdir,
// mv, chmod, the checksum and configuration queries), and creates files in
// place for opens with New. Paths are absolute; those of requests are
// decoded where EOS's encoding (/#curl#) is used.
//
// It keeps XrdCl's rules: a call that fails at once neither calls nor
// deletes its handler; an accepted one calls it exactly once, from the
// fake's own thread, in the order of submission.
//
// Faults make the next matching operations fail: at once, through the
// handler (async), or after taking effect (takes_effect, such as a close
// that commits the upload but reports an error). Every request is logged as
// "<op> <path>"; MGM commands as "proc <query>".
class FakeXrd final : public eosmirror::XrdClient, public std::enable_shared_from_this<FakeXrd> {
 public:
  using EntryType = eosmirror::EntryType;
  using ModeBits = eosmirror::ModeBits;
  using Timespec = eosmirror::Timespec;
  using Status = XrdCl::XRootDStatus;

  struct Node {
    EntryType type = EntryType::File;
    ModeBits mode = 0644;
    uint32_t uid = 0;
    uint32_t gid = 0;
    uint64_t fid = 0;  // files
    std::string content;
    Timespec mtime{1000, 0};
    Timespec ctime{1000, 0};
    std::string target;  // symlinks
  };

  // A request path or URL path, split: the path (decoded) and the opaque
  // parameters in order.
  struct Request {
    std::string path;
    std::string query;  // all after '?'
    std::vector<std::pair<std::string, std::string>> params;

    std::optional<std::string> param(std::string_view key) const {
      for (const auto& [k, v] : params)
        if (k == key) return v;
      return std::nullopt;
    }
  };

  struct Fault {
    std::string op = {};        // as in the request log: "open", "close", "query:getfmd", ...
    std::string path = {};      // empty for any
    Status status = {};         // success, for an operation that only appears to succeed
    bool async = false;         // reads and writes: through the handler
    bool takes_effect = false;  // the operation is done all the same
    int remaining = 1;
  };

  FakeXrd() : worker_([this] { work(); }) {
    Node top;
    top.type = EntryType::Directory;
    top.mode = 0755;
    nodes_["/"] = top;
  }

  ~FakeXrd() override {
    {
      std::lock_guard lock(queue_mutex_);
      stopping_ = true;
    }
    queue_cv_.notify_all();
    worker_.join();
  }

  FakeXrd(const FakeXrd&) = delete;
  FakeXrd& operator=(const FakeXrd&) = delete;

  // ---- what tests set up and look at ----------------------------------------------

  // Whether the MGM takes the client for root (whoami).
  bool root = true;
  // The reply to XRootD's configuration query for checksums: "0:adler32", or
  // "chksum" (echoed) for a server without checksums.
  std::string checksum_config = "0:adler32";
  // Whether a move onto an existing file replaces it; otherwise it fails
  // with kXR_ItExists, as on servers that refuse to replace.
  bool mv_replaces = false;
  // Replies to MGM commands other than whoami, by their query
  // ("mgm.cmd=..." or "mgm.cmd.proto=..."), as the whole reply
  // ("mgm.proc.stdout=...&mgm.proc.stderr=...&mgm.proc.retc=N"). Unanswered
  // commands get EOPNOTSUPP.
  std::function<std::optional<std::string>(std::string_view query)> proc;
  // Replies to fsctl queries (mgm.pcmd) in place of the model's, by command,
  // decoded path and the model's reply (which has taken effect); nothing to
  // keep the model's.
  std::function<std::optional<std::string>(std::string_view pcmd, const std::string& path,
                                           const std::string& reply)>
      reply;
  // Called after an upload is in place under its name.
  std::function<void(const std::string& path)> on_commit;

  void fail(Fault fault) {
    std::lock_guard lock(mutex_);
    faults_.push_back(std::move(fault));
  }

  // A directory, with its parents.
  void dir(const std::string& path, ModeBits mode = 0755) {
    std::lock_guard lock(mutex_);
    for (size_t at = 1; at <= path.size(); ++at) {
      if (at < path.size() && path[at] != '/') continue;
      Node& n = nodes_[path.substr(0, at)];
      n.type = EntryType::Directory;
      if (at == path.size()) n.mode = mode;
    }
  }

  // A file, in an existing directory. Its id.
  uint64_t file(const std::string& path, std::string content, Timespec mtime = {1000, 0},
                ModeBits mode = 0644) {
    std::lock_guard lock(mutex_);
    Node n;
    n.mode = mode;
    n.fid = ++last_fid_;
    n.content = std::move(content);
    n.mtime = n.ctime = mtime;
    nodes_[path] = n;
    return n.fid;
  }

  // Moves an entry, replacing what is at `to`, as another client may.
  void rename(const std::string& from, const std::string& to) {
    std::lock_guard lock(mutex_);
    Node n = nodes_.at(from);
    nodes_.erase(from);
    nodes_[to] = n;
  }

  void symlink(const std::string& path, std::string target) {
    std::lock_guard lock(mutex_);
    Node n;
    n.type = EntryType::Symlink;
    n.mode = 0777;
    n.target = std::move(target);
    nodes_[path] = n;
  }

  std::optional<Node> at(const std::string& path) const {
    std::lock_guard lock(mutex_);
    auto it = nodes_.find(path);
    if (it == nodes_.end()) return std::nullopt;
    return it->second;
  }

  // The paths of all entries, sorted.
  std::vector<std::string> paths() const {
    std::lock_guard lock(mutex_);
    std::vector<std::string> out;
    for (const auto& [path, node] : nodes_) out.push_back(path);
    return out;
  }

  std::vector<std::string> requests() const {
    std::lock_guard lock(mutex_);
    return log_;
  }
  void clear_requests() {
    std::lock_guard lock(mutex_);
    log_.clear();
  }

  static Status error(int kxr, const std::string& message) {
    return Status(XrdCl::stError, XrdCl::errErrorResponse, static_cast<uint32_t>(kxr), message);
  }
  static Status timeout() { return Status(XrdCl::stError, XrdCl::errOperationExpired); }

  static std::string adler32(std::string_view content) {
    eosmirror::Hasher h(eosmirror::ChecksumType::Adler32);
    h.update({reinterpret_cast<const std::byte*>(content.data()), content.size()});
    return h.finish().hex;
  }

  static std::string escape(std::string_view s) {
    std::string out;
    for (unsigned char c : s) {
      if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~' || c == '/') {
        out += static_cast<char>(c);
      } else {
        char buf[4];
        std::snprintf(buf, sizeof buf, "%%%02X", c);
        out += buf;
      }
    }
    return out;
  }

  static std::string unescape(std::string_view s) {
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
      if (s[i] == '%' && i + 2 < s.size()) {
        out += static_cast<char>(std::strtol(std::string(s.substr(i + 1, 2)).c_str(), nullptr, 16));
        i += 2;
      } else {
        out += s[i];
      }
    }
    return out;
  }

  // A request path ("/#curl#<escaped>?..." or "/plain?...") or URL
  // ("root://host:port/<request path>"), split.
  static Request parse(std::string_view text) {
    if (auto scheme = text.find("://"); scheme != std::string_view::npos) {
      auto slash = text.find('/', scheme + 3);
      text = slash == std::string_view::npos ? std::string_view() : text.substr(slash + 1);
    }
    Request r;
    auto q = text.find('?');
    std::string_view path = text.substr(0, q);
    r.path = path.starts_with("/#curl#") ? unescape(path.substr(7)) : std::string(path);
    if (q == std::string_view::npos) return r;
    r.query = std::string(text.substr(q + 1));
    for (std::string_view rest = r.query; !rest.empty();) {
      std::string_view field = rest.substr(0, rest.find('&'));
      rest.remove_prefix(std::min(rest.size(), field.size() + 1));
      auto eq = field.find('=');
      r.params.emplace_back(std::string(field.substr(0, eq)),
                            eq == std::string_view::npos ? "" : std::string(field.substr(eq + 1)));
    }
    return r;
  }

  // ---- XrdClient --------------------------------------------------------------------

  std::unique_ptr<eosmirror::XrdFileApi> new_file() override {
    return std::make_unique<File>(shared_from_this());
  }
  std::unique_ptr<eosmirror::XrdFsApi> new_file_system(const std::string&) override {
    return std::make_unique<Fs>(shared_from_this());
  }

 private:
  // The model is guarded by mutex_, which hooks may take again.
  using Lock = std::lock_guard<std::recursive_mutex>;

  static std::string parent(const std::string& path) {
    auto slash = path.rfind('/');
    return slash == 0 ? "/" : path.substr(0, slash);
  }
  static std::string name(const std::string& path) { return path.substr(path.rfind('/') + 1); }
  static std::string sealed(const std::string& path) {
    std::string out;
    for (char c : path) out += c == '&' ? std::string("#AND#") : std::string(1, c);
    return out;
  }
  static std::string hex(uint64_t v) {
    char buf[20];
    std::snprintf(buf, sizeof buf, "%llx", static_cast<unsigned long long>(v));
    return buf;
  }

  // Logs a request and returns the fault it meets, if any.
  std::optional<Fault> request(const std::string& op, const std::string& path) {
    Lock lock(mutex_);
    log_.push_back(op + " " + path);
    for (auto it = faults_.begin(); it != faults_.end(); ++it) {
      if (it->op != op || (!it->path.empty() && it->path != path)) continue;
      Fault f = *it;
      if (--it->remaining <= 0) faults_.erase(it);
      return f;
    }
    return std::nullopt;
  }

  Timespec now() {
    Lock lock(mutex_);
    return {2000000000 + ++clock_, 0};
  }

  std::string path_of(uint64_t fid) const {
    Lock lock(mutex_);
    for (const auto& [path, node] : nodes_)
      if (node.type == EntryType::File && node.fid == fid) return path;
    return {};
  }

  // Runs a handler on the fake's thread.
  void post(XrdCl::ResponseHandler* handler, const Status& status, XrdCl::AnyObject* response) {
    {
      std::lock_guard lock(queue_mutex_);
      queue_.push_back(
          [handler, status, response] { handler->HandleResponse(new Status(status), response); });
    }
    queue_cv_.notify_one();
  }

  void work() {
    for (;;) {
      std::function<void()> job;
      {
        std::unique_lock lock(queue_mutex_);
        queue_cv_.wait(lock, [&] { return stopping_ || !queue_.empty(); });
        if (queue_.empty()) return;
        job = std::move(queue_.front());
        queue_.pop_front();
      }
      job();
    }
  }

  XrdCl::StatInfo* stat_info(const Node& n) const {
    uint32_t flags = kXR_readable | kXR_writable;
    if (n.type == EntryType::Directory) flags |= kXR_isDir | kXR_xset;
    if (n.type == EntryType::Symlink) flags |= kXR_other;
    std::string id = n.type == EntryType::File ? std::to_string(n.fid) : "0";
    uint64_t size = n.type == EntryType::File ? n.content.size() : 0;
    return new XrdCl::StatInfo(id, size, flags, static_cast<uint64_t>(n.mtime.sec));
  }

  std::string whoami() const {
    std::string id =
        root ? "uid=0 (0,3,65534) gid=0 (0,4,65534)" : "uid=1000 (1000,99) gid=1000 (1000,99)";
    return "mgm.proc.stdout=Virtual Identity: " + id +
           " [authz:krb5] host=localhost domain=localdomain\n&mgm.proc.stderr=&mgm.proc.retc=0";
  }

  // The reply of an fsctl query (mgm.pcmd) on the model.
  std::string fsctl(const std::string& pcmd, const Request& r) {
    Lock lock(mutex_);
    auto it = nodes_.find(r.path);
    if (pcmd == "getfmd") {
      std::string path =
          path_of(std::strtoull(r.param("mgm.getfmd.fid").value_or("0").c_str(), nullptr, 10));
      if (path.empty()) return "getfmd: retc=2";
      const Node& n = nodes_.at(path);
      return "getfmd: retc=0 name=" + sealed(name(path)) + "&id=" + std::to_string(n.fid) +
             "&ctime=" + std::to_string(n.ctime.sec) + "&ctime_ns=" + std::to_string(n.ctime.nsec) +
             "&mtime=" + std::to_string(n.mtime.sec) + "&mtime_ns=" + std::to_string(n.mtime.nsec) +
             "&size=" + std::to_string(n.content.size()) + "&cid=1&uid=" + std::to_string(n.uid) +
             "&gid=" + std::to_string(n.gid) +
             "&lid=6553874&flags=416&link=&location=1,&checksum=" + adler32(n.content) +
             "&container=" + sealed(parent(path)) + (parent(path) == "/" ? "" : "/");
    }
    if (it == nodes_.end()) return pcmd + ": retc=2";
    Node& n = it->second;
    if (pcmd == "stat") {
      unsigned type = n.type == EntryType::Directory ? 0040000
                      : n.type == EntryType::Symlink ? 0120000
                                                     : 0100000;
      unsigned long long ino = n.type == EntryType::File ? n.fid << 28 : 1;
      char buf[400];
      std::snprintf(buf, sizeof buf, "stat: 1 %llu %u 1 %u %u 0 %zu 4096 1 %lld %lld %lld 0 %d %d",
                    ino, type | n.mode, n.uid, n.gid, n.content.size(),
                    static_cast<long long>(n.mtime.sec), static_cast<long long>(n.mtime.sec),
                    static_cast<long long>(n.ctime.sec), n.mtime.nsec, n.ctime.nsec);
      return buf;
    }
    if (pcmd == "readlink") return "readlink: retc=0 /#curl#" + escape(n.target);
    if (pcmd == "utimes") {
      n.mtime = {std::atoll(r.param("tv2_sec").value_or("0").c_str()),
                 std::atoi(r.param("tv2_nsec").value_or("0").c_str())};
      return "utimes: retc=0";
    }
    if (pcmd == "chmod") {
      n.mode = static_cast<ModeBits>(std::atoi(r.param("mode").value_or("0").c_str()));
      return "chmod: retc=0";
    }
    if (pcmd == "chown") {
      n.uid = static_cast<uint32_t>(std::atoi(r.param("uid").value_or("0").c_str()));
      n.gid = static_cast<uint32_t>(std::atoi(r.param("gid").value_or("0").c_str()));
      return "chown: retc=0";
    }
    return pcmd + ": retc=95";
  }

  class File final : public eosmirror::XrdFileApi {
   public:
    explicit File(std::shared_ptr<FakeXrd> xrd) : xrd_(std::move(xrd)) {}

    Status open(const std::string& url, XrdCl::OpenFlags::Flags flags,
                XrdCl::Access::Mode mode) override {
      Request r = parse(url);
      path_ = r.path;
      if (path_.starts_with("/proc/")) {
        std::string query = r.query.substr(0, r.query.find("&eos.app="));
        if (auto f = xrd_->request("proc", query)) return f->status;
        if (query == "mgm.cmd=whoami") {
          content_ = xrd_->whoami();
        } else {
          auto answer = xrd_->proc ? xrd_->proc(query) : std::nullopt;
          content_ = answer ? *answer
                            : "mgm.proc.stdout=&mgm.proc.stderr=not in the fake&mgm.proc.retc=95";
        }
        kind_ = Kind::Proc;
        last_url_ = url;
        open_ = true;
        return {};
      }
      auto fault = xrd_->request("open", path_);
      if (fault && !fault->takes_effect) return fault->status;
      Lock lock(xrd_->mutex_);
      auto& nodes = xrd_->nodes_;
      if (flags & XrdCl::OpenFlags::Write) {
        auto dir = nodes.find(parent(path_));
        if (dir == nodes.end() || dir->second.type != EntryType::Directory)
          return error(kXR_NotFound, "no directory for " + path_);
        Node n;
        n.fid = ++xrd_->last_fid_;
        n.mode = (static_cast<ModeBits>(mode) & 0777) | 0600;
        n.mtime = n.ctime = xrd_->now();
        if (auto m = r.param("eos.mtime")) {
          auto dot = m->find('.');
          n.mtime = {std::atoll(m->substr(0, dot).c_str()),
                     dot == std::string::npos ? 0 : std::atoi(m->substr(dot + 1).c_str())};
        }
        if (r.param("eos.atomic") == "1") {
          kind_ = Kind::Upload;
          written_ = parent(path_) + (parent(path_) == "/" ? "" : "/") + ".sys.a#." + name(path_) +
                     "." + hex(n.fid);
          mtime_set_ = r.param("eos.mtime").has_value();
        } else {
          if ((flags & XrdCl::OpenFlags::New) && nodes.count(path_))
            return error(kXR_ItExists, path_ + " exists");
          kind_ = Kind::Create;
          written_ = path_;
        }
        fid_ = n.fid;
        nodes[written_] = n;
        last_url_ = "root://fst.fake:1095/" + path_ + "?mgm.id=" + hex(fid_);
      } else {
        auto it = nodes.find(path_);
        if (it == nodes.end()) return error(kXR_NotFound, "no such file " + path_);
        if (it->second.type != EntryType::File) return error(kXR_isDirectory, path_);
        kind_ = Kind::Read;
        fid_ = it->second.fid;
        // The MGM serves empty files itself, without redirecting.
        last_url_ = it->second.content.empty()
                        ? url
                        : "root://fst.fake:1095/" + path_ + "?mgm.id=" + hex(fid_);
      }
      open_ = true;
      if (fault) return fault->status;
      return {};
    }

    Status read(uint64_t offset, uint32_t size, void* buffer, uint32_t& bytes_read) override {
      if (auto f = xrd_->request("read", path_)) return f->status;
      auto got = fetch(offset, size);
      if (!got) return error(kXR_NotFound, path_ + " is gone");
      std::copy(got->begin(), got->end(), static_cast<char*>(buffer));
      bytes_read = static_cast<uint32_t>(got->size());
      return {};
    }

    Status read(uint64_t offset, uint32_t size, void* buffer,
                XrdCl::ResponseHandler* handler) override {
      auto f = xrd_->request("read", path_);
      if (f && !f->async) return f->status;
      if (f) {
        xrd_->post(handler, f->status, nullptr);
        return {};
      }
      auto got = fetch(offset, size);
      if (!got) return error(kXR_NotFound, path_ + " is gone");
      std::copy(got->begin(), got->end(), static_cast<char*>(buffer));
      auto* response = new XrdCl::AnyObject();
      response->Set(new XrdCl::ChunkInfo(offset, static_cast<uint32_t>(got->size()), buffer));
      xrd_->post(handler, {}, response);
      return {};
    }

    Status write(uint64_t offset, uint32_t size, const void* buffer,
                 XrdCl::ResponseHandler* handler) override {
      auto f = xrd_->request("write", path_ + " " + std::to_string(offset));
      if (f && !f->async && !f->takes_effect) return f->status;
      if (!f || f->takes_effect) {
        Lock lock(xrd_->mutex_);
        auto it = xrd_->nodes_.find(written_);
        if (!open_ || it == xrd_->nodes_.end()) return error(kXR_FileNotOpen, path_);
        std::string& content = it->second.content;
        if (content.size() < offset + size) content.resize(offset + size);
        content.replace(offset, size, static_cast<const char*>(buffer), size);
      }
      xrd_->post(handler, f ? f->status : Status(), nullptr);
      return {};
    }

    Status close() override {
      auto f = xrd_->request("close", path_);
      bool was_open = std::exchange(open_, false);
      if (kind_ != Kind::Upload || !was_open) return f ? f->status : Status();
      Lock lock(xrd_->mutex_);
      auto& nodes = xrd_->nodes_;
      auto it = nodes.find(written_);
      if (discarded_ || it == nodes.end()) return error(kXR_FSError, "the upload is gone");
      if (f && !f->takes_effect) {
        nodes.erase(it);  // the storage node discards an upload it cannot close
        return f->status;
      }
      Node n = it->second;
      nodes.erase(it);
      n.ctime = xrd_->now();
      if (!mtime_set_) n.mtime = n.ctime;
      nodes[path_] = n;
      if (xrd_->on_commit) xrd_->on_commit(path_);
      return f ? f->status : Status();
    }

    Status stat(bool, XrdCl::StatInfo*& response) override {
      if (auto f = xrd_->request("fstat", path_)) return f->status;
      Lock lock(xrd_->mutex_);
      std::string at = xrd_->path_of(fid_);
      if (at.empty()) return error(kXR_NotFound, path_ + " is gone");
      response = xrd_->stat_info(xrd_->nodes_.at(at));
      return {};
    }

    Status fcntl(const XrdCl::Buffer& arg, XrdCl::Buffer*& response) override {
      std::string what = arg.ToString();
      if (auto f = xrd_->request("fcntl:" + what, path_)) return f->status;
      response = nullptr;
      if (what != "delete" || kind_ != Kind::Upload) return error(kXR_Unsupported, what);
      Lock lock(xrd_->mutex_);
      xrd_->nodes_.erase(written_);
      discarded_ = true;
      return {};
    }

    bool is_open() const override { return open_; }
    bool set_property(const std::string&, const std::string&) override { return true; }
    bool get_property(const std::string& name, std::string& value) const override {
      if (name != "LastURL" || last_url_.empty()) return false;
      value = last_url_;
      return true;
    }

   private:
    enum class Kind { None, Proc, Read, Upload, Create };

    std::optional<std::string> fetch(uint64_t offset, uint32_t size) {
      if (kind_ == Kind::Proc)
        return offset >= content_.size() ? std::string() : content_.substr(offset, size);
      Lock lock(xrd_->mutex_);
      std::string at = xrd_->path_of(fid_);
      if (at.empty()) return std::nullopt;
      const std::string& content = xrd_->nodes_.at(at).content;
      return offset >= content.size() ? std::string() : content.substr(offset, size);
    }

    std::shared_ptr<FakeXrd> xrd_;
    Kind kind_ = Kind::None;
    std::string path_;
    std::string written_;  // where an upload or a created file is written
    std::string content_;  // an MGM command's reply
    std::string last_url_;
    uint64_t fid_ = 0;
    bool open_ = false;
    bool mtime_set_ = false;
    bool discarded_ = false;
  };

  class Fs final : public eosmirror::XrdFsApi {
   public:
    explicit Fs(std::shared_ptr<FakeXrd> xrd) : xrd_(std::move(xrd)) {}

    Status query(XrdCl::QueryCode::Code code, const XrdCl::Buffer& arg,
                 XrdCl::Buffer*& response) override {
      Request r = parse(arg.ToString());
      std::string text;
      if (code == XrdCl::QueryCode::Config) {
        if (auto f = xrd_->request("query:config", r.path)) return f->status;
        text = xrd_->checksum_config;
      } else if (code == XrdCl::QueryCode::Checksum) {
        if (auto f = xrd_->request("query:checksum", r.path)) return f->status;
        auto n = xrd_->at(r.path);
        if (!n || n->type != EntryType::File) return error(kXR_NotFound, r.path);
        text = "adler32 " + adler32(n->content);
      } else {
        std::string pcmd = r.param("mgm.pcmd").value_or("");
        auto f = xrd_->request("query:" + pcmd, r.path);
        if (f && !f->takes_effect) return f->status;
        text = xrd_->fsctl(pcmd, r);
        if (xrd_->reply)
          if (auto scripted = xrd_->reply(pcmd, r.path, text)) text = *scripted;
        if (f) return f->status;
      }
      response = new XrdCl::Buffer();
      response->FromString(text);
      return {};
    }

    Status stat(const std::string& path, XrdCl::StatInfo*& response) override {
      Request r = parse(path);
      if (auto f = xrd_->request("stat", r.path)) return f->status;
      auto n = xrd_->at(r.path);
      if (!n) return error(kXR_NotFound, r.path);
      response = xrd_->stat_info(*n);
      return {};
    }

    Status dir_list(const std::string& path, XrdCl::DirListFlags::Flags flags,
                    XrdCl::ResponseHandler* handler) override {
      XrdCl::DirectoryList* listing = nullptr;
      Status st = dir_list(path, flags, listing);
      if (!st.IsOK()) return st;
      auto* response = new XrdCl::AnyObject();
      response->Set(listing);
      xrd_->post(handler, {}, response);
      return {};
    }

    Status dir_list(const std::string& path, XrdCl::DirListFlags::Flags flags,
                    XrdCl::DirectoryList*& response) override {
      Request r = parse(path);
      if (auto f =
              xrd_->request(flags & XrdCl::DirListFlags::Stat ? "dirlist:stat" : "dirlist", r.path))
        return f->status;
      Lock lock(xrd_->mutex_);
      auto it = xrd_->nodes_.find(r.path);
      if (it == xrd_->nodes_.end()) return error(kXR_NotFound, r.path);
      if (it->second.type != EntryType::Directory)
        return error(kXR_FSError, r.path + " is no directory");
      auto* listing = new XrdCl::DirectoryList();
      std::string prefix = r.path == "/" ? "/" : r.path + "/";
      for (const auto& [p, n] : xrd_->nodes_) {
        if (!p.starts_with(prefix) || p.size() == prefix.size() ||
            p.find('/', prefix.size()) != std::string::npos)
          continue;
        listing->Add(new XrdCl::DirectoryList::ListEntry(
            "fake", p.substr(prefix.size()),
            flags & XrdCl::DirListFlags::Stat ? xrd_->stat_info(n) : nullptr));
      }
      response = listing;
      return {};
    }

    Status mkdir(const std::string& path, XrdCl::MkDirFlags::Flags,
                 XrdCl::Access::Mode mode) override {
      Request r = parse(path);
      if (auto f = xrd_->request("mkdir", r.path)) return f->status;
      Lock lock(xrd_->mutex_);
      auto& nodes = xrd_->nodes_;
      if (nodes.count(r.path)) return error(kXR_ItExists, r.path);
      auto dir = nodes.find(parent(r.path));
      if (dir == nodes.end() || dir->second.type != EntryType::Directory)
        return error(kXR_NotFound, r.path);
      Node n;
      n.type = EntryType::Directory;
      n.mode = static_cast<ModeBits>(mode) & 07777;
      nodes[r.path] = n;
      return {};
    }

    Status chmod(const std::string& path, XrdCl::Access::Mode mode) override {
      Request r = parse(path);
      if (auto f = xrd_->request("chmod", r.path)) return f->status;
      Lock lock(xrd_->mutex_);
      auto it = xrd_->nodes_.find(r.path);
      if (it == xrd_->nodes_.end()) return error(kXR_NotFound, r.path);
      it->second.mode = static_cast<ModeBits>(mode) & 07777;
      return {};
    }

    Status rmdir(const std::string& path) override {
      Request r = parse(path);
      if (auto f = xrd_->request("rmdir", r.path)) return f->status;
      Lock lock(xrd_->mutex_);
      auto& nodes = xrd_->nodes_;
      auto it = nodes.find(r.path);
      if (it == nodes.end()) return error(kXR_NotFound, r.path);
      if (it->second.type != EntryType::Directory)
        return error(kXR_FSError, r.path + " is no directory");
      auto next = std::next(it);
      if (next != nodes.end() && next->first.starts_with(r.path + "/"))
        return error(kXR_ItExists, r.path + " is not empty");  // as XRootD servers reply
      nodes.erase(it);
      return {};
    }

    Status rm(const std::string& path) override {
      Request r = parse(path);
      if (auto f = xrd_->request("rm", r.path)) return f->status;
      Lock lock(xrd_->mutex_);
      auto it = xrd_->nodes_.find(r.path);
      if (it == xrd_->nodes_.end()) return error(kXR_NotFound, r.path);
      if (it->second.type == EntryType::Directory) return error(kXR_isDirectory, r.path);
      xrd_->nodes_.erase(it);
      return {};
    }

    Status mv(const std::string& source, const std::string& dest) override {
      Request from = parse(source), to = parse(dest);
      if (auto f = xrd_->request("mv", from.path + " " + to.path)) return f->status;
      Lock lock(xrd_->mutex_);
      auto& nodes = xrd_->nodes_;
      auto it = nodes.find(from.path);
      if (it == nodes.end()) return error(kXR_NotFound, from.path);
      if (nodes.count(to.path) && !xrd_->mv_replaces) return error(kXR_ItExists, to.path);
      Node n = it->second;
      nodes.erase(it);
      nodes[to.path] = n;
      return {};
    }

   private:
    std::shared_ptr<FakeXrd> xrd_;
  };

  mutable std::recursive_mutex mutex_;
  std::map<std::string, Node> nodes_;
  std::vector<Fault> faults_;
  std::vector<std::string> log_;
  uint64_t last_fid_ = 0;
  int64_t clock_ = 0;

  std::mutex queue_mutex_;
  std::condition_variable queue_cv_;
  std::deque<std::function<void()>> queue_;
  bool stopping_ = false;
  std::thread worker_;  // last: started once the rest is constructed
};
