#!/bin/bash
# A single-host EOS instance in rootless podman containers for the integration
# tests: QuarkDB, one MGM and one FST with several filesystems, started from
# CERN's public EOS image. State (secrets, namespace, data, logs) lives under
# $EOSMIRROR_EOS_STATE and survives restarts; "destroy" removes it.
#
# Usage: instance.sh init|start|configure|up|stop|destroy|status|eos [args...]
#   up         init, start and configure
#   eos ARGS   run the eos CLI as root on the MGM
# Env:   EOSMIRROR_EOS_IMAGE   image (default gitlab-registry.cern.ch/dss/eos/eos-ci:5.5.2.el9)
#        EOSMIRROR_EOS_STATE   state directory (default ${TMPDIR:-/tmp}/eosmirror-eos)
#        EOSMIRROR_EOS_SUBNET  first three octets of the test network (default 10.89.251)
#
# All containers join the podman network "eosmirror-test" with fixed addresses
# and know each other through /etc/hosts, since podman's DNS cannot answer the
# reverse lookups the XRootD servers make for every new client address.
set -euo pipefail

here=$(dirname "$(readlink -f "$0")")
IMAGE=${EOSMIRROR_EOS_IMAGE:-gitlab-registry.cern.ch/dss/eos/eos-ci:5.5.2.el9}
STATE=${EOSMIRROR_EOS_STATE:-${TMPDIR:-/tmp}/eosmirror-eos}
SUBNET=${EOSMIRROR_EOS_SUBNET:-10.89.251}
NET=eosmirror-test
DOMAIN=eosmirror.test
NFS=8
S=$STATE/secrets

declare -A IP=([qdb]=$SUBNET.11 [mgm]=$SUBNET.12 [fst]=$SUBNET.13)
HOSTS=()
for c in "${!IP[@]}"; do
  HOSTS+=(--add-host "eosmirror-$c.$DOMAIN:${IP[$c]}" --add-host "eosmirror-$c:${IP[$c]}")
done

eos() { podman exec eosmirror-mgm eos -b "$@"; }

running() { [[ $(podman inspect --format '{{.State.Running}}' "eosmirror-$1" 2>/dev/null) == true ]]; }

# Retry "$@" every 2 s until it succeeds, for at most $TIMEOUT s.
wait_for() {
  local what=$1 start=$SECONDS deadline=$((SECONDS + ${TIMEOUT:-300}))
  shift
  echo -n "Waiting for $what ..."
  until "$@" > /dev/null 2>&1; do
    if ((SECONDS > deadline)); then
      echo " timeout"
      return 1
    fi
    sleep 2
    echo -n .
  done
  echo " ok ($((SECONDS - start)) s)"
}

# Runs daemon $1 (qdb, mgm or fst) the way the eos-<daemon> services do.
run_daemon() {
  local daemon=$1
  shift
  podman run -d --name "eosmirror-$daemon" --network "$NET" --init --ip "${IP[$daemon]}" \
    --hostname "eosmirror-$daemon.$DOMAIN" --network-alias "eosmirror-$daemon.$DOMAIN" "${HOSTS[@]}" \
    --env-file "$here/config/eos_env.$daemon" \
    -v "$here/config/eos_env.$daemon:/etc/sysconfig/eos_env:ro" \
    -v "$here/config/xrd.cf.$daemon:/etc/xrd.cf.$daemon:ro" \
    -v "$STATE/log:/var/log/eos" \
    -v "$S/uid2/eos.keytab:/etc/eos.keytab:ro" \
    -v "$S/uid2/eos.keytab:/etc/eos.client.keytab:ro" \
    -v "$S/uid2/macaroon.secret:/etc/eos.macaroon.secret:ro" \
    -v "$S/uid2/qdb.password:/etc/eos/qdb.password:ro" \
    --entrypoint /bin/bash --workdir /var/eos --stop-timeout 60 \
    "$@" "$IMAGE" \
    -c "/usr/sbin/eos_start_pre.sh eos-start-pre $daemon; exec /usr/sbin/eos_start.sh -n $daemon -c /etc/xrd.cf.$daemon -l /var/log/eos/xrdlog.$daemon -Rdaemon" \
    > /dev/null
}

qdb_ready() {
  local info
  info=$(podman exec eosmirror-qdb bash -c 'REDISCLI_AUTH=$(cat /etc/eos/qdb.password) exec redis-cli -p 7777 raft-info')
  grep -Eq '^LEADER \S+:[0-9]+$' <<< "$info" && grep -q '^NODE-HEALTH GREEN$' <<< "$info"
}

mgm_ready() { grep -q 'is_master=true' <<< "$(eos ns)"; }

fst_listening() { podman exec eosmirror-fst bash -c 'exec 3<>/dev/tcp/localhost/1095'; }

fst_booted() {
  local fs
  fs=$(eos fs ls -m)
  [[ -n $fs ]] && ! grep -v 'stat.boot=booted' <<< "$fs" > /dev/null \
    && ! grep -v 'stat.active=online' <<< "$fs" > /dev/null
}

# The FST exits unless the MGM knows its node within 50 s of starting.
register_fst_node() {
  grep -q "eosmirror-fst.$DOMAIN" <<< "$(eos node ls -m)" || eos node set "eosmirror-fst.$DOMAIN:1095" on
}

do_init() {
  mkdir -p -m 0700 "$S"
  if [[ ! -e $S/eos.keytab ]]; then
    # One key for the daemons, one for clients that EOS maps to root (like
    # a migration host's key) and one for clients that may assert any
    # identity (which XRootD 6 clients leave as nobody).
    for key in "eosmirror-daemon daemon daemon" "eosmirror-root root root"       "eosmirror-client anybody anygroup"; do
      read -r name user group <<< "$key"
      podman run --rm --network=none -v "$S:/out" --entrypoint /opt/eos/xrootd/bin/xrdsssadmin \
        "$IMAGE" -k "$name" -u "$user" -g "$group" add /out/eos.keytab
    done
  fi
  [[ -e $S/eos-client.keytab ]] || grep ' n:eosmirror-client ' "$S/eos.keytab" > "$S/eos-client.keytab"
  [[ -e $S/eos-root.keytab ]] || grep ' n:eosmirror-root ' "$S/eos.keytab" > "$S/eos-root.keytab"
  for f in qdb.password macaroon.secret; do
    [[ -e $S/$f ]] || openssl rand -hex 32 | tr -d '\n' > "$S/$f"
  done
  [[ -e $S/qdb.clusterid ]] || openssl rand -hex 16 > "$S/qdb.clusterid"
  chmod 0600 "$S"/*
  # The daemons run as uid 2 in the containers.
  podman unshare bash -ec '
    mkdir -p -m 0700 "$1/uid2"
    for f in eos.keytab qdb.password macaroon.secret; do
      install -o 2 -g 2 -m 0400 "$1/$f" "$1/uid2/$f"
    done' _ "$S"

  local bays
  bays=$(seq -f "$STATE/data/bay%02g" 1 "$NFS")
  # shellcheck disable=SC2086
  mkdir -p "$STATE"/state/{qdb,mgm/ns-queue,fst} "$STATE/log" $bays
  # shellcheck disable=SC2086
  podman unshare chown 2:2 "$STATE"/state/qdb "$STATE"/state/mgm "$STATE"/state/mgm/ns-queue \
    "$STATE"/state/fst "$STATE/log" $bays
  podman unshare chmod 0700 "$STATE/state/mgm/ns-queue"

  podman network exists "$NET" || podman network create --subnet "$SUBNET.0/24" "$NET" > /dev/null

  if [[ ! -e $STATE/state/qdb/db ]]; then
    podman run --rm --network=none --user 2:2 -v "$STATE/state/qdb:/var/lib/qdb" \
      --entrypoint quarkdb-create "$IMAGE" --path=/var/lib/qdb/db \
      --clusterID="$(cat "$S/qdb.clusterid")" --nodes="eosmirror-qdb.$DOMAIN:7777"
  fi
  echo "Initialized $STATE"
}

do_start() {
  if ! running qdb; then
    podman rm -f eosmirror-qdb > /dev/null 2>&1 || true
    run_daemon qdb -v "$STATE/state/qdb:/var/lib/qdb"
  fi
  wait_for QuarkDB qdb_ready
  if ! running mgm; then
    podman rm -f eosmirror-mgm > /dev/null 2>&1 || true
    run_daemon mgm -v "$STATE/state/mgm:/var/eos"
  fi
  wait_for "MGM to be master" mgm_ready
  if ! running fst; then
    podman rm -f -t 60 eosmirror-fst > /dev/null 2>&1 || true
    register_fst_node
    run_daemon fst -v "$STATE/state/fst:/var/eos" -v "$STATE/data:/data/fst"
    wait_for "FST to listen" fst_listening
    [[ -z $(eos fs ls -m) ]] || wait_for "filesystems to boot" fst_booted
  fi
}

# Configures the instance in the MGM (idempotent): authentication, space,
# the FST's filesystems and the test directories.
do_configure() {
  local vid
  vid=$(eos vid ls)
  grep -q 'sss:"<pwd>":uid => root' <<< "$vid" || eos vid enable sss
  grep -q 'sudoer *=> uids(daemon)' <<< "$vid" || eos vid set membership 2 +sudo

  [[ -n $(eos space ls -m default) ]] || eos space define default 8 1
  local registered i bay
  registered=$(eos fs ls -m)
  for i in $(seq 1 "$NFS"); do
    bay=$(printf bay%02d "$i")
    grep -Eq "(^| )id=$i( |$)" <<< "$registered" \
      || eos fs add -m "$i" "$bay-$(openssl rand -hex 4)" "eosmirror-fst.$DOMAIN:1095" "/data/fst/$bay" default.0 rw
  done
  register_fst_node
  grep -q 'cfg.status=on' <<< "$(eos group ls -m default.0)" || eos group set default.0 on

  # Test directories: path, layout, stripes, checksum.
  while read -r dir layout nstripes checksum; do
    eos mkdir -p "$dir"
    eos chmod 777 "$dir" > /dev/null
    for attr in sys.forced.space=default "sys.forced.layout=$layout" "sys.forced.nstripes=$nstripes" \
      sys.forced.blocksize=1M sys.forced.blockchecksum=crc32c; do
      eos attr set "$attr" "$dir" > /dev/null
    done
    eos attr set "sys.forced.checksum=$checksum" "$dir" > /dev/null
  done <<EOF
/eos/test/replica2 replica 2 adler
/eos/test/raid6 raid6 6 adler
/eos/test/nochecksum replica 1 none
EOF
  wait_for "filesystems to boot" fst_booted
  wait_for "the space to accept writes" writes_ok
  eos fs ls
}

# A small upload into the test tree succeeds (placement needs a moment after
# the filesystems boot).
writes_ok() {
  podman exec eosmirror-mgm /opt/eos/xrootd/bin/xrdcp -f -s /etc/hostname \
    "root://localhost:1094//eos/test/replica2/.write-probe?eos.atomic=1"
}

do_stop() {
  for daemon in fst mgm qdb; do
    podman rm -f -t 60 "eosmirror-$daemon" > /dev/null 2>&1 || true
  done
}

case ${1:-} in
  init) do_init ;;
  start) do_start ;;
  configure) do_configure ;;
  up)
    do_init
    do_start
    do_configure
    ;;
  stop) do_stop ;;
  destroy)
    do_stop
    podman unshare rm -rf "$STATE"
    ;;
  status) podman ps --filter 'name=^eosmirror-(qdb|mgm|fst)$' --format '{{.Names}} {{.Status}}' ;;
  eos)
    shift
    eos "$@"
    ;;
  *)
    echo "usage: $0 init|start|configure|up|stop|destroy|status|eos [args...]" >&2
    exit 2
    ;;
esac
