#!/bin/bash
# Configure, build and run the tests in a build container.
#
# Usage: scripts/check.sh PLATFORM [SANITIZER] [cmake-option...]
#   PLATFORM   el9, ubuntu22 or ubuntu24 (see containers/)
#   SANITIZER  none (default), address, thread or undefined
# Env: EOSMIRROR_BUILD_ROOT as for in-container.sh. The build directory is
#      <root>/<platform>/<sanitizer>.
set -euo pipefail

here=$(dirname "$(readlink -f "$0")")
platform=${1:?usage: check.sh PLATFORM [SANITIZER] [cmake-option...]}
sanitizer=${2:-none}
shift
[[ $# -gt 0 ]] && shift

cmake_sanitizer=$sanitizer
[[ $sanitizer == none ]] && cmake_sanitizer=""

# TSan builds run the tests under `setarch -R` (see tests/CMakeLists.txt),
# which the default seccomp profile of the container runtime forbids.
podman_opts=()
[[ $sanitizer == thread ]] && podman_opts=(--security-opt=seccomp=unconfined)

exec "$here/in-container.sh" "$platform" "${podman_opts[@]}" bash -ec "
  cmake -S /src -B /build/$sanitizer -G Ninja -DEOSMIRROR_SANITIZER='$cmake_sanitizer' $*
  cmake --build /build/$sanitizer
  ctest --test-dir /build/$sanitizer --output-on-failure
"
