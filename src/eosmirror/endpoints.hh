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
// path, or a URL such as root://host//path for XRootD and EOS.
struct EndpointSettings {
  PosixOptions posix;
  XrdOptions xrootd;
};

Result<std::unique_ptr<Endpoint>> make_endpoint(const std::string& spec,
                                                const EndpointSettings& settings);

}  // namespace eosmirror
