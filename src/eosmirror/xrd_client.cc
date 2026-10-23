// SPDX-License-Identifier: GPL-3.0-or-later
#include "eosmirror/xrd_client.hh"

#include <XrdCl/XrdClURL.hh>

namespace eosmirror {

namespace {

class XrdClFile final : public XrdFileApi {
 public:
  XrdCl::XRootDStatus open(const std::string& url, XrdCl::OpenFlags::Flags flags,
                           XrdCl::Access::Mode mode) override {
    return file_.Open(url, flags, mode);
  }
  XrdCl::XRootDStatus read(uint64_t offset, uint32_t size, void* buffer,
                           uint32_t& bytes_read) override {
    return file_.Read(offset, size, buffer, bytes_read);
  }
  XrdCl::XRootDStatus read(uint64_t offset, uint32_t size, void* buffer,
                           XrdCl::ResponseHandler* handler) override {
    return file_.Read(offset, size, buffer, handler);
  }
  XrdCl::XRootDStatus write(uint64_t offset, uint32_t size, const void* buffer,
                            XrdCl::ResponseHandler* handler) override {
    return file_.Write(offset, size, buffer, handler);
  }
  XrdCl::XRootDStatus close() override { return file_.Close(); }
  XrdCl::XRootDStatus stat(bool force, XrdCl::StatInfo*& response) override {
    return file_.Stat(force, response);
  }
  XrdCl::XRootDStatus fcntl(const XrdCl::Buffer& arg, XrdCl::Buffer*& response) override {
    return file_.Fcntl(arg, response);
  }
  bool is_open() const override { return file_.IsOpen(); }
  bool set_property(const std::string& name, const std::string& value) override {
    return file_.SetProperty(name, value);
  }
  bool get_property(const std::string& name, std::string& value) const override {
    return file_.GetProperty(name, value);
  }

 private:
  XrdCl::File file_{XrdCl::File::DisableVirtRedirect};
};

class XrdClFileSystem final : public XrdFsApi {
 public:
  explicit XrdClFileSystem(const std::string& url) : fs_(XrdCl::URL(url)) {}

  XrdCl::XRootDStatus query(XrdCl::QueryCode::Code code, const XrdCl::Buffer& arg,
                            XrdCl::Buffer*& response) override {
    return fs_.Query(code, arg, response);
  }
  XrdCl::XRootDStatus stat(const std::string& path, XrdCl::StatInfo*& response) override {
    return fs_.Stat(path, response);
  }
  XrdCl::XRootDStatus dir_list(const std::string& path, XrdCl::DirListFlags::Flags flags,
                               XrdCl::ResponseHandler* handler) override {
    return fs_.DirList(path, flags, handler);
  }
  XrdCl::XRootDStatus dir_list(const std::string& path, XrdCl::DirListFlags::Flags flags,
                               XrdCl::DirectoryList*& response) override {
    return fs_.DirList(path, flags, response);
  }
  XrdCl::XRootDStatus mkdir(const std::string& path, XrdCl::MkDirFlags::Flags flags,
                            XrdCl::Access::Mode mode) override {
    return fs_.MkDir(path, flags, mode);
  }
  XrdCl::XRootDStatus chmod(const std::string& path, XrdCl::Access::Mode mode) override {
    return fs_.ChMod(path, mode);
  }
  XrdCl::XRootDStatus rmdir(const std::string& path) override { return fs_.RmDir(path); }
  XrdCl::XRootDStatus rm(const std::string& path) override { return fs_.Rm(path); }
  XrdCl::XRootDStatus mv(const std::string& source, const std::string& dest) override {
    return fs_.Mv(source, dest);
  }

 private:
  XrdCl::FileSystem fs_;
};

class XrdClClient final : public XrdClient {
 public:
  std::unique_ptr<XrdFileApi> new_file() override { return std::make_unique<XrdClFile>(); }
  std::unique_ptr<XrdFsApi> new_file_system(const std::string& url) override {
    return std::make_unique<XrdClFileSystem>(url);
  }
};

}  // namespace

std::shared_ptr<XrdClient> xrdcl_client() {
  static const std::shared_ptr<XrdClient> client = std::make_shared<XrdClClient>();
  return client;
}

}  // namespace eosmirror
