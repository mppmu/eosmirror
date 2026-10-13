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

# The real run, which logs the endpoints as resolved.
ln -s src "$work/srclink"
"$bin" sync "${owner_opt[@]}" --journal "$work/j.db" "$work/srclink/" "file://$work/dst" > "$work/run.out" 2> "$work/run.err"
grep -q "source: $(readlink -f "$work/src")\$" "$work/run.err" || fail "source not logged: $(cat "$work/run.err")"
grep -q "target: $(readlink -f "$work")/dst\$" "$work/run.err" || fail "target not logged: $(cat "$work/run.err")"
[[ $(sha256sum < "$work/src/big") == $(sha256sum < "$work/dst/big") ]] || fail "big differs"
[[ $(readlink "$work/dst/link") == a ]] || fail "symlink"
[[ $(stat -c %Y "$work/dst/a") == $(stat -c %Y "$work/src/a") ]] || fail "mtime of a"
[[ $(stat -c %Y "$work/dst/sub/deep") == $(stat -c %Y "$work/src/sub/deep") ]] || fail "mtime of dir"
grep -q 'failures: 0' "$work/run.out" || fail "summary: $(cat "$work/run.out")"

# Dry runs neither create nor change a journal, and resume what the real run
# would resume: nothing after a completed run.
"$bin" sync -n -q "${owner_opt[@]}" --journal "$work/new.db" "$work/src" "$work/dst" > /dev/null \
  || fail "dry run without its journal"
[[ ! -e $work/new.db ]] || fail "a dry run created its journal"
journal_state() { ls "$work" | grep '^j\.db'; sha256sum < "$work/j.db"; }
before=$(journal_state)
"$bin" sync -n "${owner_opt[@]}" --resume --journal "$work/j.db" "$work/src" "$work/dst" > "$work/dry.out" 2>&1 \
  || fail "dry run with the journal: $(cat "$work/dry.out")"
grep -q 'directories: 3 listed' "$work/dry.out" || fail "dry --resume after a completed run: $(cat "$work/dry.out")"
[[ $(journal_state) == "$before" ]] || fail "a dry run changed the journal"

# Journals in the trees they record, and files that are not journals, are
# refused.
rc=0; "$bin" sync -q "${owner_opt[@]}" --journal "$work/dst/j.db" "$work/src" "$work/dst" > /dev/null 2> "$work/inside.err" || rc=$?
[[ $rc == 3 && ! -e $work/dst/j.db ]] && grep -q 'is in the tree of' "$work/inside.err" \
  || fail "journal in the target: $rc $(cat "$work/inside.err")"
echo notes > "$work/notes.db"
rc=0; "$bin" failures "$work/notes.db" 2> "$work/notes.err" || rc=$?
[[ $rc == 3 && $(cat "$work/notes.db") == notes ]] && grep -q 'not an eosmirror journal' "$work/notes.err" \
  || fail "failures of a file that is not a journal: $rc $(cat "$work/notes.err")"

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

# With --keep-emptied, the copy of an empty source directory (as an unmounted
# automount looks) keeps its entries. Without, the copy is emptied.
mkdir "$work/src/automount" "$work/dst/automount"
touch "$work/dst/automount/f1" "$work/dst/automount/f2"
"$bin" sync -q "${owner_opt[@]}" --delete --keep-emptied "$work/src" "$work/dst" > "$work/emptied.out" 2> "$work/emptied.err" \
  || fail "emptied directory: $(cat "$work/emptied.err")"
[[ -f $work/dst/automount/f2 ]] && grep -q 'deleting nothing in automount' "$work/emptied.err" \
  || fail "emptied directory deleted: $(cat "$work/emptied.err")"
grep -q 'empty on the source whose target entries were kept (--keep-emptied): 1' "$work/emptied.out" || fail "emptied summary"
"$bin" sync -q "${owner_opt[@]}" --delete "$work/src" "$work/dst" > /dev/null
[[ -z $(ls -A "$work/dst/automount") ]] || fail "emptied directory kept"

# An entry that cannot be copied (a name with a line break) is a failure of
# its directory, recorded in the journal, as rsync exits with 23.
mkdir -p "$work/odd/sub"
touch "$work/odd/sub/fine" "$work/odd/sub/$(printf 'line\nbreak')"
rc=0; "$bin" sync -q "${owner_opt[@]}" --journal "$work/odd.db" "$work/odd" "$work/odd-copy" > /dev/null 2>&1 || rc=$?
[[ $rc == 1 && -f $work/odd-copy/sub/fine ]] || fail "an entry that cannot be copied: $rc"
"$bin" failures "$work/odd.db" 2> /dev/null | grep -q $'^directory\tsub\t' || fail "its directory not recorded"

# --checksum: a file rewritten with its size and mtime is found by a dry run
# (exit 1, recorded in the journal) and copied by a retry of the failures.
mkdir -p "$work/ck/src"
echo "same size" > "$work/ck/src/f"
echo other > "$work/ck/src/g"
"$bin" sync -q "${owner_opt[@]}" "$work/ck/src" "$work/ck/dst" > /dev/null || fail "copy to verify"
echo "SAME SIZE" > "$work/ck/dst/f"
touch -r "$work/ck/src/f" "$work/ck/dst/f"
rc=0; "$bin" sync -n -c "${owner_opt[@]}" --journal "$work/ck.db" "$work/ck/src" "$work/ck/dst" > "$work/ck.out" 2>&1 || rc=$?
[[ $rc == 1 ]] && grep -q 'f: differs from the source: adler32' "$work/ck.out" \
  && grep -q 'compared by checksum: 2, differing: 1' "$work/ck.out" || fail "verification: $rc $(cat "$work/ck.out")"
[[ $(cat "$work/ck/dst/f") == "SAME SIZE" ]] || fail "a verification changed the target"
"$bin" sync -q "${owner_opt[@]}" --journal "$work/ck.db" --retry-failed "$work/ck/src" "$work/ck/dst" > /dev/null || fail "repair"
[[ $(cat "$work/ck/dst/f") == "same size" ]] || fail "not repaired"
"$bin" sync -n -c -q "${owner_opt[@]}" --journal "$work/ck.db" "$work/ck/src" "$work/ck/dst" > /dev/null || fail "verified again"

# An empty or missing source does not empty the target.
mkdir "$work/empty"
rc=0; "$bin" sync -q "${owner_opt[@]}" --delete "$work/empty" "$work/dst" > /dev/null 2> "$work/empty.err" || rc=$?
[[ $rc == 3 ]] && grep -q 'refusing to delete' "$work/empty.err" || fail "empty source: $rc $(cat "$work/empty.err")"
rc=0; "$bin" sync -q "${owner_opt[@]}" --delete "$work/missing" "$work/dst" > /dev/null 2>&1 || rc=$?
[[ $rc == 3 ]] || fail "missing source should exit 3, got $rc"
[[ -f $work/dst/a ]] || fail "target emptied"

# Trees that overlap are refused before anything is created or deleted:
# a target around the source, inside it, or the same tree by another name.
mkdir -p "$work/data/proj" "$work/data/unrelated"
echo p > "$work/data/proj/f"
echo u > "$work/data/unrelated/g"
ln -s data/proj "$work/projlink"
for args in "--delete --dry-run $work/data/proj $work/data" "--delete $work/data/proj $work/data" \
  "$work/data/proj $work/data/proj/mirror/deeper" "$work/projlink file://$work/data/proj/" \
  "$work/data/proj/ $work/projlink/../proj"; do
  rc=0; "$bin" sync -q "${owner_opt[@]}" $args > /dev/null 2> "$work/overlap.err" || rc=$?
  [[ $rc == 3 ]] && grep -Eq 'same|inside' "$work/overlap.err" || fail "overlap $args: $rc $(cat "$work/overlap.err")"
done
[[ -f $work/data/unrelated/g && ! -e $work/data/proj/mirror ]] || fail "an overlapping run changed something"
"$bin" sync -q "${owner_opt[@]}" "$work/data/proj" "$work/data/project" > /dev/null || fail "proj -> project"

# Failures in a row stop the run with exit 4.
mkdir -p "$work/conflicts/src" "$work/conflicts/dst"
for i in 1 2 3 4 5 6; do
  echo $i > "$work/conflicts/src/f$i"
  mkdir "$work/conflicts/dst/f$i"
done
rc=0; "$bin" sync -q "${owner_opt[@]}" --journal "$work/stop.db" --max-consecutive-failures 3 "$work/conflicts/src" "$work/conflicts/dst" > /dev/null 2> "$work/stop.err" || rc=$?
[[ $rc == 4 ]] && grep -q '3 failures in a row' "$work/stop.err" || fail "stop after failures: $rc $(cat "$work/stop.err")"
# Resuming it needs the same options.
rc=0; "$bin" sync -q "${owner_opt[@]}" --journal "$work/stop.db" --resume --delete "$work/conflicts/src" "$work/conflicts/dst" > /dev/null 2> "$work/resume.err" || rc=$?
[[ $rc == 3 ]] && grep -q 'other --delete$' "$work/resume.err" || fail "resume with other options: $rc $(cat "$work/resume.err")"
rc=0; "$bin" sync -q "${owner_opt[@]}" --max-consecutive-failures unlimited "$work/conflicts/src" "$work/conflicts/dst" > /dev/null 2>&1 || rc=$?
[[ $rc == 1 ]] || fail "unlimited failures should exit 1, got $rc"

echo "smoke test passed"
