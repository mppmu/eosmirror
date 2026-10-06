#!/bin/bash
# End-to-end check of the eosmirror binary on local directories: exit codes,
# the journal, dry runs and the failures listing.
#
# Usage: smoke.sh PATH_TO_EOSMIRROR
set -euo pipefail

bin=${1:?usage: smoke.sh EOSMIRROR}
work=$(mktemp -d "${TMPDIR:-/tmp}/eosmirror-smoke-XXXXXX")
trap 'rm -rf "$work"' EXIT

fail() { echo "FAIL: $*" >&2; exit 1; }

owner_opt=()
[[ $(id -u) == 0 ]] || owner_opt=(--no-owner)

mkdir -p "$work/src/sub/deep"
echo hello > "$work/src/a"
head -c 300000 /dev/urandom > "$work/src/big"
echo deep > "$work/src/sub/deep/f"
ln -s a "$work/src/link"
touch -d '2020-01-02 03:04:05' "$work/src/a" "$work/src/sub/deep"

"$bin" --version | grep -q '^eosmirror ' || fail "--version"
rc=0; "$bin" sync 2>/dev/null || rc=$?
[[ $rc == 2 ]] || fail "usage error should exit 2, got $rc"
rc=0; "$bin" sync "$work/src" 2>/dev/null || rc=$?
[[ $rc == 2 ]] || fail "usage error exit code, got $rc"

# Paths below /eos name an EOS instance, which needs an MGM.
rc=0; env -u EOS_MGM_URL "$bin" sync -q /eos/x "$work/eosdst" 2> "$work/eos.err" || rc=$?
[[ $rc == 3 ]] && grep -q 'EOS_MGM_URL' "$work/eos.err" || fail "/eos path without an MGM: $rc $(cat "$work/eos.err")"
rc=0; env -u EOS_MGM_URL "$bin" sync -q "$work/src" /eos/x/y/z 2> "$work/eos.err" || rc=$?
[[ $rc == 3 ]] && grep -q '^.*/eos/x/y/z is an EOS path' "$work/eos.err" || fail "/eos target without an MGM: $rc $(cat "$work/eos.err")"

# The self-test passes on a local directory and cleans up.
mkdir "$work/target"
"$bin" selftest "${owner_opt[@]}" "$work/target" > "$work/selftest.out" || fail "selftest: $(cat "$work/selftest.out")"
grep -q 'The target is ready' "$work/selftest.out" || fail "selftest output: $(cat "$work/selftest.out")"
[[ -z $(ls -A "$work/target") ]] || fail "selftest left files behind"

# A dry run creates nothing.
"$bin" sync -n -q "${owner_opt[@]}" "$work/src" "$work/dst" > "$work/dry.out"
[[ -e $work/dst ]] && fail "dry run created the target"
grep -q '3 would be copied' "$work/dry.out" || fail "dry run summary: $(cat "$work/dry.out")"

# The real run.
"$bin" sync -q "${owner_opt[@]}" --journal "$work/j.db" "$work/src" "$work/dst" > "$work/run.out"
[[ $(sha256sum < "$work/src/big") == $(sha256sum < "$work/dst/big") ]] || fail "big differs"
[[ $(readlink "$work/dst/link") == a ]] || fail "symlink"
[[ $(stat -c %Y "$work/dst/a") == $(stat -c %Y "$work/src/a") ]] || fail "mtime of a"
[[ $(stat -c %Y "$work/dst/sub/deep") == $(stat -c %Y "$work/src/sub/deep") ]] || fail "mtime of dir"
grep -q 'failures: 0' "$work/run.out" || fail "summary: $(cat "$work/run.out")"

# Missing parents of the target are created, also for relative paths and
# for shards that start together.
"$bin" sync -q "${owner_opt[@]}" "$work/src" "$work/deep/er/dst" > /dev/null || fail "sync into missing parents"
[[ -f $work/deep/er/dst/a ]] || fail "parents not created"
(cd "$work" && "$bin" sync -q "${owner_opt[@]}" src rel/dst > /dev/null) || fail "sync into a relative path"
[[ -f $work/rel/dst/a ]] || fail "relative parents not created"
pids=()
for k in 0 1 2; do
  "$bin" sync -q "${owner_opt[@]}" --shard $k/3 "$work/src" "$work/sharded/a/b/dst" > /dev/null &
  pids+=($!)
done
for pid in "${pids[@]}"; do wait "$pid" || fail "a shard failed"; done
"$bin" sync -q "${owner_opt[@]}" "$work/src" "$work/sharded/a/b/dst" > "$work/shards.out"
grep -q 'files: 0 copied' "$work/shards.out" || fail "shards left work: $(cat "$work/shards.out")"

# A rerun copies nothing.
"$bin" sync -q "${owner_opt[@]}" "$work/src" "$work/dst" > "$work/rerun.out"
grep -q 'files: 0 copied' "$work/rerun.out" || fail "rerun summary: $(cat "$work/rerun.out")"

# A type conflict is a failure: exit 1, recorded in the journal, listed,
# and resolved by retrying with --delete.
echo file > "$work/src/sub/conflict"
mkdir "$work/dst/sub/conflict"
rc=0; "$bin" sync -q "${owner_opt[@]}" --retries 0 --journal "$work/j.db" "$work/src" "$work/dst" > "$work/fail.out" || rc=$?
[[ $rc == 1 ]] || fail "expected exit 1, got $rc"
"$bin" failures "$work/j.db" 2>/dev/null | grep -q 'sub/conflict' || fail "failure not listed"
"$bin" sync -q "${owner_opt[@]}" --journal "$work/j.db" --retry-failed --delete "$work/src" "$work/dst" > "$work/retry.out" || fail "retry failed"
[[ -f $work/dst/sub/conflict ]] || fail "conflict not resolved"
[[ -z $("$bin" failures "$work/j.db" 2>/dev/null) ]] || fail "failure not cleared"

# Deletion needs the option and respects the cap.
touch "$work/dst/extra1" "$work/dst/extra2"
"$bin" sync -q "${owner_opt[@]}" "$work/src" "$work/dst" > /dev/null
[[ -e $work/dst/extra1 ]] || fail "deleted without --delete"
rc=0; "$bin" sync -q "${owner_opt[@]}" --delete --max-delete 1 "$work/src" "$work/dst" > /dev/null || rc=$?
[[ $rc == 1 ]] || fail "cap should exit 1, got $rc"
[[ $(ls "$work/dst" | grep -c extra) == 1 ]] || fail "cap not respected"
"$bin" sync -q "${owner_opt[@]}" --delete "$work/src" "$work/dst" > /dev/null
[[ $(ls "$work/dst" | grep -c extra) == 0 ]] || fail "extra not deleted"

# An empty or missing source does not empty the target.
mkdir "$work/empty"
rc=0; "$bin" sync -q "${owner_opt[@]}" --delete "$work/empty" "$work/dst" > /dev/null 2> "$work/empty.err" || rc=$?
[[ $rc == 3 ]] && grep -q 'refusing to delete' "$work/empty.err" || fail "empty source: $rc $(cat "$work/empty.err")"
rc=0; "$bin" sync -q "${owner_opt[@]}" --delete "$work/missing" "$work/dst" > /dev/null 2>&1 || rc=$?
[[ $rc == 3 ]] || fail "missing source should exit 3, got $rc"
[[ -f $work/dst/a ]] || fail "target emptied"

# Failures in a row stop the run with exit 4.
mkdir -p "$work/conflicts/src" "$work/conflicts/dst"
for i in 1 2 3 4 5 6; do
  echo $i > "$work/conflicts/src/f$i"
  mkdir "$work/conflicts/dst/f$i"
done
rc=0; "$bin" sync -q "${owner_opt[@]}" --max-consecutive-failures 3 "$work/conflicts/src" "$work/conflicts/dst" > /dev/null 2> "$work/stop.err" || rc=$?
[[ $rc == 4 ]] && grep -q '3 failures in a row' "$work/stop.err" || fail "stop after failures: $rc $(cat "$work/stop.err")"
rc=0; "$bin" sync -q "${owner_opt[@]}" --max-consecutive-failures unlimited "$work/conflicts/src" "$work/conflicts/dst" > /dev/null 2>&1 || rc=$?
[[ $rc == 1 ]] || fail "unlimited failures should exit 1, got $rc"

echo "smoke test passed"
