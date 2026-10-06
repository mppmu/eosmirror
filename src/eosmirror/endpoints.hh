// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <memory>
#include <string>

#include "eosmirror/endpoint.hh"
#include "eosmirror/error.hh"
#include "eosmirror/posix_endpoint.hh"
#include "eosmirror/xrootd_endpoint.hh"

namespace eosmirror {

// Settings for the endpoints a command line argument can name: a local
// path, a URL such as root://host//path for XRootD and EOS, or a path below
// /eos on the EOS instance of the MGM.
struct EndpointSettings {
  PosixOptions posix;
  XrdOptions xrootd;
  std::string mgm;  // the MGM for /eos paths, root://host[:port]; else EOS_MGM_URL
};

// The URL of a command line path below /eos, "<mgm>//eos/...", with the MGM
// from mgm or else env_mgm (EOS_MGM_URL, as for the eos client). Other specs
// come back unchanged; a local EOS FUSE mount is named as file:///eos/....
Result<std::string> eos_path_url(const std::string& spec, const std::string& mgm,
                                 const char* env_mgm);

// The endpoint a command line argument names. URLs of XRootD servers are
// probed for an EOS MGM.
Result<std::unique_ptr<Endpoint>> make_endpoint(const std::string& spec,
                                                const EndpointSettings& settings);

}  // namespace eosmirror
