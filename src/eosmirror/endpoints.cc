// SPDX-License-Identifier: GPL-3.0-or-later
#include "eosmirror/endpoints.hh"

#include <netdb.h>
#include <sys/socket.h>

#include <algorithm>
#include <cstdlib>
#include <mutex>
#include <set>
#include <string_view>

#include "eosmirror/log.hh"

#ifdef EOSMIRROR_HAVE_XROOTD
#include "eosmirror/eos_endpoint.hh"
#include "eosmirror/xrootd_endpoint.hh"
#endif

namespace eosmirror {

#ifdef EOSMIRROR_HAVE_XROOTD
namespace {

// Logs what kind of server an endpoint is on, once per server.
void log_server(const std::string& url, std::string_view kind) {
  static std::mutex mutex;
  static std::set<std::string> logged;
  auto parts = parse_endpoint_url(url);
  std::string server = display_url(parts.ok() ? parts.value().server : url);
  std::lock_guard lock(mutex);
  if (logged.insert(server).second) log::info(server, ": ", kind);
}

}  // namespace
#endif

namespace {

// The addresses of a host, numeric; none if it cannot be resolved.
std::set<std::string> addresses(std::string host) {
  if (host.size() > 2 && host.front() == '[' && host.back() == ']')
    host = host.substr(1, host.size() - 2);
  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  addrinfo* found = nullptr;
  std::set<std::string> result;
  if (getaddrinfo(host.c_str(), nullptr, &hints, &found) != 0) return result;
  for (const addrinfo* a = found; a; a = a->ai_next) {
    char text[NI_MAXHOST];
    if (getnameinfo(a->ai_addr, a->ai_addrlen, text, sizeof text, nullptr, 0, NI_NUMERICHOST) == 0)
      result.insert(text);
  }
  freeaddrinfo(found);
  return result;
}

// Whether two servers ("host:port") are one: the same port, and the same
// name or an address in common (an alias, a short name, an address).
bool same_server(const std::string& a, const std::string& b) {
  if (a == b) return true;
  auto colon_a = a.rfind(':');
  auto colon_b = b.rfind(':');
  if (colon_a == std::string::npos || colon_b == std::string::npos ||
      a.substr(colon_a) != b.substr(colon_b))
    return false;
  std::set<std::string> of_a = addresses(a.substr(0, colon_a));
  std::set<std::string> of_b = addresses(b.substr(0, colon_b));
  return std::any_of(of_a.begin(), of_a.end(), [&](const std::string& x) { return of_b.count(x); });
}

}  // namespace

std::string to_string(const Location& location) {
  return location.server.empty() ? location.path : location.path + " on " + location.server;
}

bool contains(const Location& outer, const Location& inner) {
  if (outer.server != inner.server) return false;
  const std::string& o = outer.path;
  const std::string& i = inner.path;
  return i == o || o == "/" || (i.starts_with(o) && i[o.size()] == '/');
}

Status check_disjoint(const Endpoint& source, const Endpoint& target) {
  Location src = source.location();
  Location dst = target.location();
  if (!src.server.empty() && !dst.server.empty() && same_server(src.server, dst.server))
    dst.server = src.server;
  std::string problem;
  if (contains(src, dst) && contains(dst, src))
    problem = "source and target are the same, " + to_string(src);
  else if (contains(src, dst))
    problem = "the target " + to_string(dst) + " is inside the source " + to_string(src);
  else if (contains(dst, src))
    problem = "the source " + to_string(src) + " is inside the target " + to_string(dst);
  if (problem.empty()) return {};
  return Error{ErrorKind::Other, problem + ": refusing to run"};
}

bool is_eos_path(const std::string& spec) { return spec == "/eos" || spec.starts_with("/eos/"); }

std::string describe_resolved(const std::string& spec, const Endpoint& endpoint,
                              const EndpointSettings& settings) {
  std::string text = to_string(endpoint.location());
  if (is_eos_path(spec))
    text += " (" + spec + " on the MGM from " + (settings.mgm.empty() ? "EOS_MGM_URL" : "--mgm") +
            ")";
  return text;
}

Result<std::string> eos_path_url(const std::string& spec, const std::string& mgm,
                                 const char* env_mgm) {
  if (!is_eos_path(spec)) return spec;
  // XRootD takes what follows a '?' for opaque parameters: another
  // directory would be synchronized.
  if (spec.find('?') != std::string::npos)
    return Error{ErrorKind::Other, spec + " contains a '?', which would end the path and start "
                                          "the opaque parameters of its URL"};
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
        log_server(url, "EOS instance");
        return std::unique_ptr<Endpoint>(std::move(ep).value());
      }
      auto ep = XrdEndpoint::create(url, settings.xrootd);
      if (!ep.ok()) return ep.error();
      log_server(url, "XRootD server");
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
