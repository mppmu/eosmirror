#!/bin/bash
# Starts or stops a plain XRootD server in a container for the integration
# tests. The server joins the podman network "eosmirror-test" under the name
# "xrootd", so that containers on that network reach it as root://xrootd:1094.
# /data/fixtures holds a tree with symlink loops.
#
# Usage: xrootd-server.sh start|stop|status
# Env:   EOSMIRROR_EOS_IMAGE  image with the xrootd server under
#                             /opt/eos/xrootd/bin (default: CERN's EOS image)
set -euo pipefail

here=$(dirname "$(readlink -f "$0")")
image=${EOSMIRROR_EOS_IMAGE:-gitlab-registry.cern.ch/dss/eos/eos-ci:5.5.2.el9}
network=eosmirror-test
name=eosmirror-xrootd

case ${1:-} in
  start)
    podman network exists "$network" || podman network create "$network" > /dev/null
    podman rm -f "$name" > /dev/null 2>&1 || true
    podman run -d --name "$name" --network "$network" --network-alias xrootd \
      -v "$here/xrootd.cf:/etc/xrootd.cf:ro" --entrypoint /bin/bash "$image" -c '
        mkdir -p /data/fixtures/loop/sub
        echo fixture > /data/fixtures/loop/sub/f
        ln -s .. /data/fixtures/loop/sub/up
        ln -s . /data/fixtures/loop/self
        chown -R daemon:daemon /data
        exec /opt/eos/xrootd/bin/xrootd -R daemon -c /etc/xrootd.cf' > /dev/null
    for i in $(seq 1 30); do
      if podman exec "$name" /opt/eos/xrootd/bin/xrdfs root://localhost:1094 stat /data > /dev/null 2>&1; then
        echo "xrootd server running as $name on network $network (root://xrootd:1094//data)"
        exit 0
      fi
      sleep 1
    done
    echo "xrootd server did not come up:" >&2
    podman logs "$name" >&2
    exit 1
    ;;
  stop)
    podman rm -f "$name" > /dev/null 2>&1 || true
    ;;
  status)
    podman ps --filter "name=^$name$" --format '{{.Names}} {{.Status}}'
    ;;
  *)
    echo "usage: $0 start|stop|status" >&2
    exit 2
    ;;
esac
