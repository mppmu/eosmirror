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

echo "smoke test passed"
