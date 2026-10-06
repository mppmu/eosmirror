#!/bin/bash
# Run a command in a build container with the repository mounted at /src and a
# per-platform build directory at /build (the working directory).
#
# Usage: scripts/in-container.sh PLATFORM [--podman-option...] COMMAND [ARG...]
# Env:   EOSMIRROR_BUILD_ROOT  parent of the build directories
#                              (default: ${TMPDIR:-/tmp}/eosmirror-build)
set -euo pipefail

repo=$(readlink -f "$(dirname "$(readlink -f "$0")")/..")
platform=${1:?usage: in-container.sh PLATFORM COMMAND...}
shift

podman_opts=()
while [[ $# -gt 0 && $1 == --* ]]; do
  podman_opts+=("$1")
  shift
done

build_root=${EOSMIRROR_BUILD_ROOT:-${TMPDIR:-/tmp}/eosmirror-build}
build_dir=$build_root/$platform
mkdir -p "$build_dir"

tty_opt=()
[[ -t 0 && -t 1 ]] && tty_opt=(-it)

exec podman run --rm "${tty_opt[@]}" "${podman_opts[@]}" \
  -v "$repo:/src" -v "$build_dir:/build" -w /build \
  "localhost/eosmirror-build:$platform" "$@"
