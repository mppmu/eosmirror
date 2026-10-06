#!/bin/sh
# Runs a command as nobody when running as root, as in the build containers,
# so that tests of what users other than root cannot do run there too. Where
# nobody cannot run it, the test is skipped (exit status 77).
if [ "$(id -u)" != 0 ]; then exec "$@"; fi
as_nobody="setpriv --reuid=65534 --regid=65534 --clear-groups"
if ! $as_nobody test -x "$1" 2> /dev/null; then
  echo "skipped: nobody cannot run $1" >&2
  exit 77
fi
exec $as_nobody "$@"
