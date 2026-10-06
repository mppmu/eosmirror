// SPDX-License-Identifier: GPL-3.0-or-later
#include "eosmirror/endpoints.hh"

#include <cstdlib>

#include "eosmirror/log.hh"

#ifdef EOSMIRROR_HAVE_XROOTD
#include "eosmirror/eos_endpoint.hh"
#include "eosmirror/xrootd_endpoint.hh"
#endif

namespace eosmirror {

Result<std::string> eos_path_url(const std::string& spec, const std::string& mgm,
                                 const char* env_mgm) {
  if (spec != "/eos" && !spec.starts_with("/eos/")) return spec;
  std::string server = !mgm.empty() ? mgm : env_mgm ? env_mgm : "";
  while (!server.empty() && server.back() == '/') server.pop_back();
  if (server.empty())
    return Error{ErrorKind::Other, spec + " is an EOS path: set EOS_MGM_URL or --mgm, or use " +
                                       "root://host/" + spec};
  return server + "/" + spec;
}

Result<std::unique_ptr<Endpoint>> make_endpoint(const std::string& given,
                                                const EndpointSettings& settings) {
  if (given.empty()) return Error{ErrorKind::Other, "empty endpoint"};
  auto resolved = eos_path_url(given, settings.mgm, std::getenv("EOS_MGM_URL"));
  if (!resolved.ok()) return resolved.error();
  const std::string& spec = resolved.value();
  auto scheme_end = spec.find("://");
  if (scheme_end != std::string::npos) {
    std::string scheme = spec.substr(0, scheme_end);
    if (scheme == "file") return std::unique_ptr<Endpoint>(new PosixEndpoint(spec.substr(scheme_end + 3), settings.posix));
    bool eos = scheme == "eos";
    if (eos || scheme == "root" || scheme == "roots" || scheme == "xroot" || scheme == "xroots") {
#ifdef EOSMIRROR_HAVE_XROOTD
      // An EOS MGM answers the eos client's commands; other servers get the
      // plain XRootD endpoint. "eos://" names EOS explicitly.
      std::string url = eos ? "root" + spec.substr(3) : spec;
      bool is_eos = eos;
      if (!is_eos) {
        auto probed = EosEndpoint::is_eos(url);
        if (!probed.ok()) return probed.error();
        is_eos = probed.value();
      }
      if (is_eos) {
        auto ep = EosEndpoint::create(url, settings.xrootd);
        if (!ep.ok()) return ep.error();
        log::info(ep.value()->describe(), ": EOS instance");
        return std::unique_ptr<Endpoint>(std::move(ep).value());
      }
      auto ep = XrdEndpoint::create(url, settings.xrootd);
      if (!ep.ok()) return ep.error();
      log::info(ep.value()->describe(), ": XRootD server");
      return std::unique_ptr<Endpoint>(std::move(ep).value());
#else
      return Error{ErrorKind::Unsupported, "built without XRootD support: " + spec};
#endif
    }
    return Error{ErrorKind::Unsupported, "unknown URL scheme in " + spec};
  }
  return std::unique_ptr<Endpoint>(new PosixEndpoint(spec, settings.posix));
}

}  // namespace eosmirror
