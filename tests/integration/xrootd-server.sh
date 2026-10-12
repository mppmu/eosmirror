#!/bin/bash
# Starts or stops a plain XRootD server for the integration tests, with
# /data/fixtures holding a tree with symlink loops, a directory with
# symlinks that cannot be stated (ELOOP) and one with a name that forges a
# line of a listing.
#
# By default, the server runs in a container that joins the podman network
# "eosmirror-test" under the name "xrootd", so that containers on that
# network reach it as root://xrootd:1094//data. With EOSMIRROR_XROOTD_LOCAL=1
# it is the xrootd installed on this host instead (such as a distribution's
# xrootd-server package), run as the current user with its data below
# EOSMIRROR_XROOTD_DIR, as root://localhost:1094//data.
#
# Usage: xrootd-server.sh start|stop|status
# Env:   EOSMIRROR_EOS_IMAGE     image with the xrootd server under
#                                /opt/eos/xrootd/bin (default: CERN's EOS image)
#        EOSMIRROR_XROOTD_LOCAL  1: run the local xrootd
#        EOSMIRROR_XROOTD_DIR    the local server's directory
#                                (default ${TMPDIR:-/tmp}/eosmirror-xrootd)
set -euo pipefail

here=$(dirname "$(readlink -f "$0")")
image=${EOSMIRROR_EOS_IMAGE:-gitlab-registry.cern.ch/dss/eos/eos-ci:5.5.2.el9}
network=eosmirror-test
name=eosmirror-xrootd
local_dir=${EOSMIRROR_XROOTD_DIR:-${TMPDIR:-/tmp}/eosmirror-xrootd}

fixtures='
  mkdir -p "$1/fixtures/loop/sub"
  echo fixture > "$1/fixtures/loop/sub/f"
  ln -sfn .. "$1/fixtures/loop/sub/up"
  ln -sfn . "$1/fixtures/loop/self"
  mkdir -p "$1/fixtures/eloop" "$1/fixtures/forged"
  for i in 0 1 2; do ln -sfn "loop$i" "$1/fixtures/eloop/loop$i"; done
  for i in $(seq 0 19); do echo $i > "$1/fixtures/eloop/f$i"; done
  echo f0 > "$1/fixtures/forged/f0"
  echo x > "$1/fixtures/forged/$(printf "x\n0 999 0 1600000000\nf0")"'

# Waits until the server answers, running the given xrdfs command, while
# the server's process (pid $1, if not empty) lives.
wait_until_up() {
  local pid=$1
  shift
  for _ in $(seq 1 30); do
    [[ -z $pid ]] || kill -0 "$pid" 2> /dev/null || return 1
    # A server that does not listen yet is no reason for XrdCl to retry.
    if XRD_CONNECTIONWINDOW=2 XRD_CONNECTIONRETRY=1 "$@" stat /data > /dev/null 2>&1; then
      return 0
    fi
    sleep 1
  done
  return 1
}

# The local server's pid, if it still runs (xrootd writes xrootd.pid itself).
local_pid() {
  local pid
  [[ -s $local_dir/server.pid ]] && pid=$(cat "$local_dir/server.pid") || return 1
  [[ $(cat "/proc/$pid/comm" 2> /dev/null) == xrootd ]] && echo "$pid"
}

stop_local() {
  local pid
  if pid=$(local_pid) && kill "$pid" 2> /dev/null; then
    for _ in $(seq 1 30); do kill -0 "$pid" 2> /dev/null || break; sleep 0.2; done
  fi
  rm -f "$local_dir/server.pid"
}

start_local() {
  stop_local
  mkdir -p "$local_dir"
  # A fresh data directory; /data on the server is $local_dir/data.
  [[ -d $local_dir/data ]] && find "$local_dir/data" -mindepth 1 -delete
  mkdir -p "$local_dir/data"
  bash -c "$fixtures" _ "$local_dir/data"
  { grep -v -e '^all.adminpath' -e '^all.pidpath' "$here/xrootd.cf"
    echo "all.adminpath $local_dir"
    echo "all.pidpath $local_dir"
    echo "oss.localroot $local_dir"; } > "$local_dir/xrootd.cf"
  # xrootd refuses to run as root.
  local user=()
  if [[ $(id -u) == 0 ]]; then
    chown -R daemon:daemon "$local_dir"
    user=(-R daemon)
  fi
  setsid xrootd "${user[@]}" -c "$local_dir/xrootd.cf" -l "$local_dir/xrootd.log" \
    < /dev/null > "$local_dir/xrootd.out" 2>&1 &
  echo $! > "$local_dir/server.pid"
  if wait_until_up "$!" xrdfs root://localhost:1094; then
    echo "xrootd server running locally (root://localhost:1094//data in $local_dir)"
    return 0
  fi
  echo "xrootd server did not come up:" >&2
  cat "$local_dir"/xrootd.out "$local_dir"/xrootd.log* >&2 || true
  stop_local
  return 1
}

start_container() {
  podman network exists "$network" || podman network create "$network" > /dev/null
  podman rm -f "$name" > /dev/null 2>&1 || true
  podman run -d --name "$name" --network "$network" --network-alias xrootd \
    -v "$here/xrootd.cf:/etc/xrootd.cf:ro" --entrypoint /bin/bash "$image" -c "
      set -e
      $fixtures
      chown -R daemon:daemon /data
      exec /opt/eos/xrootd/bin/xrootd -R daemon -c /etc/xrootd.cf" _ /data > /dev/null
  if wait_until_up "" podman exec "$name" /opt/eos/xrootd/bin/xrdfs root://localhost:1094; then
    echo "xrootd server running as $name on network $network (root://xrootd:1094//data)"
    return 0
  fi
  echo "xrootd server did not come up:" >&2
  podman logs "$name" >&2
  return 1
}

local=${EOSMIRROR_XROOTD_LOCAL:-0}
case ${1:-} in
  start)
    if [[ $local == 1 ]]; then start_local; else start_container; fi
    ;;
  stop)
    if [[ $local == 1 ]]; then stop_local; else podman rm -f "$name" > /dev/null 2>&1 || true; fi
    ;;
  status)
    if [[ $local == 1 ]]; then
      pid=$(local_pid) && kill -0 "$pid" 2> /dev/null && echo "xrootd running locally as pid $pid"
    else
      podman ps --filter "name=^$name$" --format '{{.Names}} {{.Status}}'
    fi
    ;;
  *)
    echo "usage: $0 start|stop|status" >&2
    exit 2
    ;;
esac
