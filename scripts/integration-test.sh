#!/bin/bash
# Runs the integration tests of an existing build against servers started in
# containers: an XRootD server for now.
#
# Usage: scripts/integration-test.sh PLATFORM [SANITIZER] [ctest-option...]
# Env:   EOSMIRROR_BUILD_ROOT, EOSMIRROR_EOS_IMAGE as for the other scripts.
set -euo pipefail

here=$(dirname "$(readlink -f "$0")")
repo=$(readlink -f "$here/..")
platform=${1:?usage: integration-test.sh PLATFORM [SANITIZER] [ctest-option...]}
sanitizer=${2:-none}
shift
[[ $# -gt 0 ]] && shift

"$repo/tests/integration/xrootd-server.sh" start
trap '"$repo/tests/integration/xrootd-server.sh" stop' EXIT

podman_opts=(--network=eosmirror-test --env=EOSMIRROR_XROOTD_URL=root://xrootd:1094//data)
[[ $sanitizer == thread ]] && podman_opts+=(--security-opt=seccomp=unconfined)

"$here/in-container.sh" "$platform" "${podman_opts[@]}" \
  ctest --test-dir "/build/$sanitizer" -R integration --output-on-failure "$@"
