// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>

#include "eosmirror/endpoint.hh"
#include "eosmirror/pipeline.hh"

namespace XrdCl {
class XRootDStatus;
}  // namespace XrdCl

namespace eosmirror {

class XrdClient;
class XrdFileApi;
class XrdFsApi;

struct XrdOptions {
  // Writes and reads of one file kept in flight, each of a chunk of the
  // buffer size. xrdcp keeps 4 of 8 MiB. Writes are sent in offset order
  // over one connection, which erasure-coded EOS files need (see
  // docs/design.md).
  int write_window = 4;
  int read_window = 4;
  // Each thread sends its requests (file opens and their reads and writes,
  // stats, listings, MGM commands) over connections of its own, to the
  // redirector and to the storage nodes, since a server executes the
  // requests of one connection one after the other. When shared, XrdCl uses
  // one connection per server for all of them. A file always goes over one
  // connection per server.
  bool shared_connection = false;
  // The tree has no symlinks (--target-without-symlinks). Only then is a
  // file that the server refuses to rename another over, as it refuses for
  // a symlink, moved aside and removed: removing a symlink removes the file
  // it points to (XrdOssUnlink.cc, BreakLink).
  bool without_symlinks = false;
};

// The parts of an endpoint URL, proto://[user@]host[:port]//path[?cgi].
struct EndpointUrl {
  std::string server;  // proto://[user@]host:port
  std::string host;    // host:port, the host in lower case
  std::string path;    // absolute, without trailing slash
  std::string cgi;     // the opaque parameters, without '?'
};

// An error for paths that are not absolute or have '.' or '..' components.
Result<EndpointUrl> parse_endpoint_url(const std::string& url);

// A request path or URL with opaque parameters added after its own.
std::string with_cgi(std::string request, std::string_view cgi);

// The path of a request to an XRootD server. XRootD has no escaping for
// paths: a '?' starts the opaque parameters, so paths with one are refused.
Result<std::string> xrootd_request_path(const std::string& abs_path);

// The opaque parameter that every request to EOS carries (see docs/design.md).
inline constexpr std::string_view kEosApp = "&eos.app=eosmirror";

// A path for a request to EOS: percent-encoded behind "/#curl#", which the
// MGM decodes when the request carries eos.encodepath. The request path adds
// that parameter and kEosApp; further ones follow after '&'.
std::string eos_encoded_path(const std::string& abs_path);
std::string eos_request_path(const std::string& abs_path);

// Renames `from` to `to` through `move`, for servers whose move fails with
// Exists where `to` exists: the file there is first moved aside to a
// temporary name, then removed once `from` is in place, or moved back if
// that fails.
Status move_replacing(const std::string& from, const std::string& to,
                      const std::function<Status(const std::string&, const std::string&)>& move,
                      const std::function<Status(const std::string&)>& remove);

// Converts an XrdCl status into an error with the given context.
Error xrd_error(const XrdCl::XRootDStatus& status, const std::string& context);

// How paths are written in requests: as they are (XRootD), or encoded so that
// any name can be sent (EOS, eos_request_path()).
enum class PathEncoding { XRootD, Eos };

// The XRootD protocol side of an endpoint on a server, shared by the XRootD
// and EOS endpoints: the server and root of the URL, the connections, and
// the requests on absolute paths that both send the same way. The URL's
// opaque parameters go with every request. Nothing is sent before the first
// request. Used from many threads at once.
class XrdSession final {
 public:
  // url: as given, for display. client: what sends the requests, by default
  // XrdCl (xrdcl_client()).
  XrdSession(const std::string& url, EndpointUrl parts, XrdOptions options, PathEncoding encoding,
             std::shared_ptr<XrdClient> client);
  ~XrdSession();
  XrdSession(const XrdSession&) = delete;
  XrdSession& operator=(const XrdSession&) = delete;

  // The URL as given, for display; the server, root://[user[:password]@]
  // host:port; the URL's opaque parameters; the options.
  const std::string& url() const { return url_; }
  const std::string& server() const { return server_; }
  const std::string& cgi() const { return cgi_; }
  const XrdOptions& options() const { return options_; }

  // The path part of the URL, without trailing slash, until it is set to
  // another (EOS resolves it on creation). Not to be set while requests run.
  const std::string& root() const { return root_; }
  void set_root(std::string root) { root_ = std::move(root); }
  // The host and the root, normalized.
  Location location() const;

  // The absolute path on the server of a relative path, and its URL for
  // messages.
  std::string absolute(const RelPath& path) const;
  std::string url_of(const std::string& abs_path) const;

  // An absolute path as sent to the server, with the URL's opaque
  // parameters, in the session's encoding.
  Result<std::string> request_path(const std::string& abs_path) const;
  // The URL for File::Open of a request path, over the calling thread's
  // own connection unless shared.
  std::string open_url(const std::string& request_path) const;
  // The file system for the calling thread's requests, over its own
  // connection unless shared.
  XrdFsApi& fs();

  // A file object that opens files as they are (see XrdClient::new_file).
  std::unique_ptr<XrdFileApi> new_file();
  // One for an upload, with write recovery turned off.
  std::unique_ptr<XrdFileApi> new_write_file();
  // A file opened for reading.
  Result<std::unique_ptr<XrdFileApi>> open_file(const std::string& abs_path);

  // Requests on absolute paths. The result of a stat (kXR_stat, which does
  // not tell symlinks) is named by the last path component. A stored
  // checksum of a type eosmirror cannot compute, or none, has type None.
  Result<Entry> stat_abs(const std::string& abs_path);
  Status mkdir_abs(const std::string& abs_path, ModeBits mode);
  Status chmod_abs(const std::string& abs_path, ModeBits mode);
  Status rmdir_abs(const std::string& abs_path);
  Status remove_abs(const std::string& abs_path);
  // A single move, which fails with Exists where the server renames nothing
  // over `to`; rename_abs() then replaces it as move_replacing() does.
  Status move_abs(const std::string& from, const std::string& to);
  Status rename_abs(const std::string& from, const std::string& to);
  Result<Checksum> query_checksum(const std::string& abs_path);

  // A directory under the root, as XRootD lists it (dirlist with stat
  // information, and of the names alone; see the definition).
  Result<Listing> list(const RelPath& dir);

 private:
  std::string url_;
  std::string server_;
  std::string host_;  // host:port
  std::string root_;
  std::string cgi_;
  XrdOptions options_;
  PathEncoding encoding_;
  std::shared_ptr<XrdClient> client_;
  std::mutex fs_mutex_;
  std::unordered_map<int, std::unique_ptr<XrdFsApi>> fs_;  // by thread
};

// A file read through one XrdCl file with up to `window` chunk reads in
// flight (ReadAhead), held by the XRootD and EOS readers. XrdCl may call a
// read's handler after the file is closed, so its destruction waits for the
// reads in flight before it closes the file. The handlers refer to this
// object, which therefore cannot be copied or moved.
class XrdReadFile final {
 public:
  // url: the file, for messages.
  XrdReadFile(std::unique_ptr<XrdFileApi> file, std::string url, int window);
  ~XrdReadFile();
  XrdReadFile(const XrdReadFile&) = delete;
  XrdReadFile& operator=(const XrdReadFile&) = delete;

  // As FileReader::read and read_chunk.
  Result<size_t> read(uint64_t offset, std::span<std::byte> buf);
  Result<Chunk> read_chunk(uint64_t offset, uint64_t end, BufferPool& pool);

  // The server's stat of the open file: a fresh one, or else the last one
  // XrdCl got. Until a fresh one is asked for, that is the one that came with
  // the open (XrdCl asks every open for it, kXR_retstat).
  Result<Entry> stat(bool fresh);

 private:
  Status submit(uint64_t offset, std::span<std::byte> buf, ReadAhead::Done done);

  std::unique_ptr<XrdFileApi> file_;
  std::string url_;
  ReadAhead ahead_;  // last: destroyed first, while the file and url are alive
};

// A file written through one XrdCl file with up to `window` writes in
// flight, each at the end of the previous one (WriteWindow), held by the
// XRootD and EOS writers. The file must come from new_write_file(). The
// handlers refer to this object, which therefore cannot be copied or moved;
// its destruction waits for the writes in flight.
class XrdWriteFile final {
 public:
  // name: the file, for messages.
  XrdWriteFile(std::unique_ptr<XrdFileApi> file, std::string name, int window);
  ~XrdWriteFile();
  XrdWriteFile(const XrdWriteFile&) = delete;
  XrdWriteFile& operator=(const XrdWriteFile&) = delete;

  // As FileWriter::write.
  Status write(Chunk chunk);
  // Waits for the writes in flight. Fails if one of them failed or if the
  // number of bytes written is not `size`.
  Status complete(uint64_t size);
  // Closes the file after complete(), which commits it on the server.
  Status close();
  // Waits for the writes in flight and closes the file, whatever fails.
  void close_quietly();

  // For writers that end a file in their own way: the file, null once it is
  // closed or dropped; waiting for the writes in flight, which XrdCl needs
  // before a close; and dropping the file, which XrdCl closes if it is open.
  XrdFileApi* file() { return file_.get(); }
  Status drain() { return writes_.drain(); }
  void drop();

  const std::string& name() const { return name_; }

 private:
  std::unique_ptr<XrdFileApi> file_;
  std::string name_;
  WriteWindow writes_;  // last: destroyed first, while the file and name are alive
};

}  // namespace eosmirror
