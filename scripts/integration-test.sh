#!/bin/bash
# Runs the integration tests of an existing build against servers started in
# containers: a plain XRootD server and a single-host EOS instance.
#
# Usage: scripts/integration-test.sh PLATFORM [SANITIZER] [ctest-option...]
# Env:   EOSMIRROR_BUILD_ROOT, EOSMIRROR_EOS_IMAGE, EOSMIRROR_EOS_STATE as for
#        the other scripts. EOSMIRROR_KEEP_EOS=1 leaves the EOS instance
#        running afterwards (it takes a while to start).
set -euo pipefail

here=$(dirname "$(readlink -f "$0")")
repo=$(readlink -f "$here/..")
platform=${1:?usage: integration-test.sh PLATFORM [SANITIZER] [ctest-option...]}
sanitizer=${2:-none}
shift
[[ $# -gt 0 ]] && shift

eos=$repo/tests/integration/eos/instance.sh
state=${EOSMIRROR_EOS_STATE:-${TMPDIR:-/tmp}/eosmirror-eos}
subnet=${EOSMIRROR_EOS_SUBNET:-10.89.251}

cleanup() {
  "$repo/tests/integration/xrootd-server.sh" stop
  [[ ${EOSMIRROR_KEEP_EOS:-0} == 1 ]] || "$eos" stop
}
trap cleanup EXIT

"$eos" up
"$repo/tests/integration/xrootd-server.sh" start

# The test container authenticates with the instance's root key, which EOS
# maps to root, and resolves the daemons through /etc/hosts.
podman_opts=(
  --network=eosmirror-test
  --add-host=eosmirror-mgm.eosmirror.test:$subnet.12 --add-host=eosmirror-fst.eosmirror.test:$subnet.13
  --env=EOSMIRROR_XROOTD_URL=root://xrootd:1094//data
  --env=EOSMIRROR_EOS_URL=root://eosmirror-mgm.eosmirror.test//eos/test
  --env=XrdSecPROTOCOL=sss --env=XrdSecSSSKT=/etc/eos.client.keytab
  --volume="$state/secrets/eos-root.keytab:/etc/eos.client.keytab:ro"
)
[[ $sanitizer == thread ]] && podman_opts+=(--security-opt=seccomp=unconfined)

"$here/in-container.sh" "$platform" "${podman_opts[@]}" \
  ctest --test-dir "/build/$sanitizer" -R integration --output-on-failure "$@"
