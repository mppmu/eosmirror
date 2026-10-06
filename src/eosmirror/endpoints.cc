// SPDX-License-Identifier: GPL-3.0-or-later
#include "eosmirror/endpoints.hh"

namespace eosmirror {

Result<std::unique_ptr<Endpoint>> make_endpoint(const std::string& spec,
                                                const EndpointSettings& settings) {
  if (spec.empty()) return Error{ErrorKind::Other, "empty endpoint"};
  auto scheme_end = spec.find("://");
  if (scheme_end != std::string::npos) {
    std::string scheme = spec.substr(0, scheme_end);
    if (scheme == "file") return std::unique_ptr<Endpoint>(new PosixEndpoint(spec.substr(scheme_end + 3), settings.posix));
    if (scheme == "root" || scheme == "roots" || scheme == "xroot" || scheme == "xroots")
      return Error{ErrorKind::Unsupported, "XRootD endpoints are not implemented yet: " + spec};
    return Error{ErrorKind::Unsupported, "unknown URL scheme in " + spec};
  }
  return std::unique_ptr<Endpoint>(new PosixEndpoint(spec, settings.posix));
}

}  // namespace eosmirror
