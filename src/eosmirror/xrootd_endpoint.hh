// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

#include "eosmirror/endpoint.hh"
#include "eosmirror/pipeline.hh"

namespace XrdCl {
class File;
class FileSystem;
class XRootDStatus;
}  // namespace XrdCl

namespace eosmirror {

struct XrdOptions {
  // Writes and reads of one file kept in flight, each of a chunk of the
  // buffer size; xrdcp keeps 4 of 8 MiB. Writes are sent in offset order
  // over one connection, which erasure-coded EOS files need (see
  // docs/design.md).
  int write_window = 4;
  int read_window = 4;
  // Each thread sends its requests (file opens and their reads and writes,
  // stats, listings, MGM commands) over connections of its own, to the
  // redirector and to the storage nodes, since a server executes the
  // requests of one connection one after the other. Shared, XrdCl uses one
  // connection per server for all of them. A file always goes over one
  // connection per server.
  bool shared_connection = false;
};

// The URL without what may carry credentials: a password before the host and
// the opaque parameters (authz tokens and the like).
std::string display_url(std::string_view url);

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

// Renames from to to through move, for servers whose move fails with Exists
// where to exists: the file there is moved aside to a temporary name first,
// and removed once from is in place, or moved back if that fails.
Status move_replacing(const std::string& from, const std::string& to,
                      const std::function<Status(const std::string&, const std::string&)>& move,
                      const std::function<Status(const std::string&)>& remove);

// An endpoint on an XRootD server, given as root://host[:port]//path, or
// roots:// for TLS. Opaque parameters of the URL (such as authz tokens) go
// with every request.
//
// Plain XRootD has no symlinks, owners or settable mtimes, so this endpoint
// reports those as missing capabilities; the EOS endpoint adds them through
// MGM commands. Files are written under a temporary name and renamed into
// place on commit, with the checksum verified by a checksum query when the
// server computes one.
class XrdEndpoint : public Endpoint {
 public:
  static Result<std::unique_ptr<XrdEndpoint>> create(const std::string& url,
                                                     XrdOptions options = XrdOptions());
  ~XrdEndpoint() override;

  std::string describe() const override { return url_; }
  // The root normalized; EOS resolves it on creation.
  Location location() const override;
  Capabilities capabilities() const override { return caps_; }
  bool is_temporary(const Entry& entry) const override;

  Result<Entry> stat(const RelPath& path) override;
  Result<Listing> list(const RelPath& dir) override;
  Status mkdir(const RelPath& path, ModeBits mode) override;
  Status symlink(const RelPath& path, const std::string& target) override;
  Status set_metadata(const RelPath& path, const Entry& metadata, MetaFields fields) override;
  Status remove(const RelPath& path, EntryType type) override;
  Result<std::unique_ptr<FileReader>> open_read(const RelPath& path) override;
  Result<std::unique_ptr<FileWriter>> open_write(const RelPath& path,
                                                 const CommitSpec& spec) override;
  // Asks the server, if it computes checksums.
  Result<Checksum> stored_checksum(const RelPath& path, const Entry& listed) override;

  // The absolute path on the server of a relative path, and its URL for
  // messages.
  std::string absolute(const RelPath& path) const;
  std::string url_of(const std::string& abs_path) const {
    return display_url(server_) + "/" + abs_path;
  }

  // An absolute path as sent to the server, with the URL's opaque
  // parameters. EOS encodes paths and adds the opaque parameter that says so.
  virtual Result<std::string> request_path(const std::string& abs_path) const;

  // Operations on absolute server paths, used by the writer. A stored
  // checksum of a type eosmirror cannot compute, or none, has type None.
  // The result of a stat is named by the last path component.
  virtual Result<Entry> stat_abs(const std::string& abs_path);
  Result<Checksum> query_checksum(const std::string& abs_path);
  Status remove_abs(const std::string& abs_path);
  Status rename_abs(const std::string& from, const std::string& to);
  Status chmod_abs(const std::string& abs_path, ModeBits mode);

  // Converts an XrdCl status into an error with the given context.
  static Error xrd_error(const XrdCl::XRootDStatus& status, const std::string& context);

  // A file object that opens files as they are: by default, XrdCl takes
  // files named *.meta4 or *.metalink for Metalink documents and opens the
  // files they list instead (URL::IsMetalink).
  static std::unique_ptr<XrdCl::File> new_file();
  // One for an upload, with write recovery turned off.
  static std::unique_ptr<XrdCl::File> new_write_file();

 protected:
  XrdEndpoint(const std::string& url, EndpointUrl parts, XrdOptions options);

  // The URL for File::Open of a request path, over the calling thread's
  // own connection unless shared.
  std::string open_url(const std::string& request_path) const;

  // The file system for the calling thread's requests, over its own
  // connection unless shared.
  XrdCl::FileSystem& fs();

  // Opens a file for reading.
  Result<std::unique_ptr<XrdCl::File>> open_file(const std::string& abs_path);

  std::string url_;     // as given, for display (display_url)
  std::string server_;  // root://[user[:password]@]host:port
  std::string host_;    // host:port
  std::string root_;    // the path part, without trailing slash
  std::string cgi_;     // the URL's opaque parameters
  XrdOptions options_;
  Capabilities caps_;

 private:
  std::mutex fs_mutex_;
  std::unordered_map<int, std::unique_ptr<XrdCl::FileSystem>> fs_;  // by thread
};

// A file read through one XrdCl::File with up to `window` chunk reads in
// flight (ReadAhead); the base of the XRootD and EOS readers. XrdCl may call
// a read's handler after the file is closed, so the reads in flight are
// waited for first.
class XrdReader : public FileReader {
 public:
  // url: the file, for messages.
  XrdReader(std::unique_ptr<XrdCl::File> file, std::string url, int window);
  ~XrdReader() override;

  Result<size_t> read(uint64_t offset, std::span<std::byte> buf) override;
  Result<Chunk> read_chunk(uint64_t offset, uint64_t end, BufferPool& pool) override;
  Result<Entry> stat() override { return file_stat(true); }

 protected:
  // The server's stat of the open file: a fresh one, or else the last one
  // XrdCl got, which until a fresh one is the one that came with the open
  // (XrdCl asks every open for it, kXR_retstat).
  Result<Entry> file_stat(bool fresh);

 private:
  Status submit(uint64_t offset, std::span<std::byte> buf, ReadAhead::Done done);

  std::unique_ptr<XrdCl::File> file_;
  std::string url_;
  ReadAhead ahead_;
};

// A file written through one XrdCl::File with up to `window` writes in
// flight, each at the end of the previous one (WriteWindow); the base of the
// XRootD and EOS writers. The file must come from new_write_file().
class XrdFileWriter : public FileWriter {
 public:
  // name: the file, for messages.
  XrdFileWriter(std::unique_ptr<XrdCl::File> file, std::string name, int window);
  ~XrdFileWriter() override;

  Status write(Chunk chunk) override;

 protected:
  // Waits for the writes in flight; an error if one failed or if not size
  // bytes were written.
  Status complete(uint64_t size);
  // Closes the file after complete(), which commits it on the server.
  Status close();
  // Waits for the writes in flight and closes the file, whatever fails.
  void close_quietly();

  std::unique_ptr<XrdCl::File> file_;
  std::string name_;
  WriteWindow writes_;
};

}  // namespace eosmirror
