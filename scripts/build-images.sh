#!/bin/bash
# Build the container images used by check.sh: one per platform in containers/.
#
# Usage: scripts/build-images.sh [platform...]   (default: all)
set -euo pipefail

repo=$(dirname "$(readlink -f "$0")")/..
cd "$repo/containers"

platforms=("$@")
if [[ ${#platforms[@]} == 0 ]]; then
  for f in Containerfile.*; do platforms+=("${f#Containerfile.}"); done
fi

for p in "${platforms[@]}"; do
  podman build --layers -t "localhost/eosmirror-build:$p" -f "Containerfile.$p" .
done
