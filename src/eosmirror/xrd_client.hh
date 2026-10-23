// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <XrdCl/XrdClFile.hh>
#include <XrdCl/XrdClFileSystem.hh>
#include <XrdCl/XrdClXRootDResponses.hh>

#include <cstdint>
#include <memory>
#include <string>

namespace eosmirror {

// The XrdCl calls of the XRootD and EOS endpoints, behind interfaces that a
// test can implement in place of a server (XrdCl's File and FileSystem have
// no virtual methods). They take XrdCl's own types and keep its rules, which
// the endpoints rely on: a call with a handler that fails at once neither
// calls nor deletes the handler; one accepted calls it exactly once, from an
// XrdCl thread, and the handler deletes itself. Each method does what the
// XrdCl method of the same name does, with its default timeout.

// One file, as XrdCl::File.
class XrdFileApi {
 public:
  virtual ~XrdFileApi() = default;

  virtual XrdCl::XRootDStatus open(const std::string& url, XrdCl::OpenFlags::Flags flags,
                                   XrdCl::Access::Mode mode = XrdCl::Access::None) = 0;
  virtual XrdCl::XRootDStatus read(uint64_t offset, uint32_t size, void* buffer,
                                   uint32_t& bytes_read) = 0;
  virtual XrdCl::XRootDStatus read(uint64_t offset, uint32_t size, void* buffer,
                                   XrdCl::ResponseHandler* handler) = 0;
  virtual XrdCl::XRootDStatus write(uint64_t offset, uint32_t size, const void* buffer,
                                    XrdCl::ResponseHandler* handler) = 0;
  virtual XrdCl::XRootDStatus close() = 0;
  virtual XrdCl::XRootDStatus stat(bool force, XrdCl::StatInfo*& response) = 0;
  virtual XrdCl::XRootDStatus fcntl(const XrdCl::Buffer& arg, XrdCl::Buffer*& response) = 0;
  virtual bool is_open() const = 0;
  virtual bool set_property(const std::string& name, const std::string& value) = 0;
  virtual bool get_property(const std::string& name, std::string& value) const = 0;
};

// The requests on paths of one server, over one connection, as
// XrdCl::FileSystem.
class XrdFsApi {
 public:
  virtual ~XrdFsApi() = default;

  virtual XrdCl::XRootDStatus query(XrdCl::QueryCode::Code code, const XrdCl::Buffer& arg,
                                    XrdCl::Buffer*& response) = 0;
  virtual XrdCl::XRootDStatus stat(const std::string& path, XrdCl::StatInfo*& response) = 0;
  virtual XrdCl::XRootDStatus dir_list(const std::string& path, XrdCl::DirListFlags::Flags flags,
                                       XrdCl::ResponseHandler* handler) = 0;
  virtual XrdCl::XRootDStatus dir_list(const std::string& path, XrdCl::DirListFlags::Flags flags,
                                       XrdCl::DirectoryList*& response) = 0;
  virtual XrdCl::XRootDStatus mkdir(const std::string& path, XrdCl::MkDirFlags::Flags flags,
                                    XrdCl::Access::Mode mode) = 0;
  virtual XrdCl::XRootDStatus chmod(const std::string& path, XrdCl::Access::Mode mode) = 0;
  virtual XrdCl::XRootDStatus rmdir(const std::string& path) = 0;
  virtual XrdCl::XRootDStatus rm(const std::string& path) = 0;
  virtual XrdCl::XRootDStatus mv(const std::string& source, const std::string& dest) = 0;
};

// Makes the files and file systems. Shared by an endpoint and its readers
// and writers, and used from many threads at once.
class XrdClient {
 public:
  virtual ~XrdClient() = default;

  // A file that opens files as they are: XrdCl takes files named *.meta4 or
  // *.metalink for Metalink documents by default and opens the files they
  // list instead (File::DisableVirtRedirect).
  virtual std::unique_ptr<XrdFileApi> new_file() = 0;
  // The file system of the server in url, whose opaque parameters go with
  // the login.
  virtual std::unique_ptr<XrdFsApi> new_file_system(const std::string& url) = 0;
};

// The client that sends the calls to XrdCl, the same for every endpoint.
std::shared_ptr<XrdClient> xrdcl_client();

}  // namespace eosmirror
