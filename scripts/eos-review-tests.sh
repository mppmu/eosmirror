#!/bin/bash
# End-to-end tests of eosmirror against a live target, as an ordinary user
# (--no-owner). Written for the code review of mppmu/eosmirror (dev branch).
#
# Usage:
#   eos-review-tests.sh EOSMIRROR_BINARY TARGET_BASE_URL [WORKDIR]
# Example:
#   kinit skluth            # or whatever authentication your instance uses
#   ./eos-review-tests.sh ./build/src/eosmirror \
#       root://eostest.mpp.mpg.de//eos/test/user/s/skluth/eosmirror-review
#
# Env:
#   BIG_MB=1024       total size of the large files for the interruption tests
#                     (needs about 3 x BIG_MB of local disk in WORKDIR)
#   FULL_METADATA=1   check mtimes and symlinks (EOS, local); 0 for plain XRootD
#   INTERRUPT_AFTER=4 seconds before the SIGINT/SIGKILL in T7/T8
#
# Each test prints PASS, FAIL or INFO. "FINDING" lines mark the behavior the
# review reported; they are expected to show up until the code is fixed.
# Everything is written below TARGET_BASE_URL/run-<timestamp> and WORKDIR.
set -uo pipefail

BIN=${1:?usage: $0 EOSMIRROR_BINARY TARGET_BASE_URL [WORKDIR]}
BASE=${2:?usage: $0 EOSMIRROR_BINARY TARGET_BASE_URL [WORKDIR]}
WORK=${3:-$(mktemp -d "${TMPDIR:-/tmp}/eosmirror-review.XXXXXX")}
BIG_MB=${BIG_MB:-1024}
FULL_METADATA=${FULL_METADATA:-1}
BIN=$(readlink -f "$BIN")
RUN="$BASE/run-$(date +%Y%m%d-%H%M%S)-$$"
LOG="$WORK/log"
mkdir -p "$LOG"
pass=0 fail=0 findings=0

say()  { printf '%s\n' "$*" | tee -a "$LOG/results.txt"; }
ok()   { pass=$((pass + 1)); say "PASS  $*"; }
bad()  { fail=$((fail + 1)); say "FAIL  $*"; }
note() { say "INFO  $*"; }
finding() { findings=$((findings + 1)); say "FINDING  $*"; }

# Runs eosmirror, logging to $LOG/<name>.out and .err; sets $rc.
em() {
  local name=$1; shift
  "$BIN" "$@" > "$LOG/$name.out" 2> "$LOG/$name.err"
  rc=$?
  echo "+ eosmirror $* -> exit $rc" >> "$LOG/commands.txt"
}
# A number from a summary: count <name> <regex with one group>.
count() { grep -oE "$2" "$LOG/$1.out" | head -1 | grep -oE '[0-9]+' | head -1; }

# Compares two local trees: content and symlink targets, permission bits
# of all entries and, with FULL_METADATA, their mtimes (ns).
same_tree() {
  local a=$1 b=$2 what=$3 fmt='%p %y %m\n'
  [[ $FULL_METADATA == 1 ]] && fmt='%p %y %m %T@\n'
  if ! diff -r --no-dereference "$a" "$b" > "$LOG/diff-$what.txt" 2>&1; then
    bad "$what: contents differ (see $LOG/diff-$what.txt)"; return 1
  fi
  (cd "$a" && find . -mindepth 1 -printf "$fmt" | sort) > "$LOG/meta-a-$what.txt"
  (cd "$b" && find . -mindepth 1 -printf "$fmt" | sort) > "$LOG/meta-b-$what.txt"
  if ! diff "$LOG/meta-a-$what.txt" "$LOG/meta-b-$what.txt" > "$LOG/metadiff-$what.txt"; then
    bad "$what: modes or mtimes differ (see $LOG/metadiff-$what.txt)"; return 1
  fi
  ok "$what: trees identical"
}

mkfile() {  # mkfile PATH BYTES
  mkdir -p "$(dirname "$1")"
  head -c "$2" /dev/urandom > "$1"
}

say "eosmirror review tests $(date -Is)"
say "binary: $BIN ($("$BIN" --version 2>&1))"
say "target: $RUN"
say "workdir: $WORK"

# ---- the source tree -------------------------------------------------------------
SRC="$WORK/src"
CHUNK=$((8 * 1024 * 1024))
mkfile "$SRC/empty" 0
mkfile "$SRC/one" 1
mkfile "$SRC/chunk-1" $((CHUNK - 1))
mkfile "$SRC/chunk" $CHUNK
mkfile "$SRC/chunk+1" $((CHUNK + 1))
mkfile "$SRC/multi" $((5 * CHUNK + 12345))
for i in $(seq 1 200); do mkfile "$SRC/many/f$i" $((i * 37)); done
mkfile "$SRC/deep/a/b/c/d/e/f/g/h/leaf" 4096
mkfile "$SRC/names/with space" 10
mkfile "$SRC/names/per%cent" 10
mkfile "$SRC/names/amp&and" 10
mkfile "$SRC/names/hash#tag" 10
mkfile "$SRC/names/umlaut-äöü-日本" 10
mkfile "$SRC/names/-leading-dash" 10
mkfile "$SRC/names/$(printf 'L%.0s' $(seq 1 240))" 10
mkfile "$SRC/modes/ro" 10 && chmod 0444 "$SRC/modes/ro"
mkfile "$SRC/modes/exec" 10 && chmod 0755 "$SRC/modes/exec"
mkfile "$SRC/modes/private" 10 && chmod 0600 "$SRC/modes/private"
mkdir -p "$SRC/modes/rodir" && mkfile "$SRC/modes/rodir/x" 5 && chmod 0555 "$SRC/modes/rodir"
if [[ $FULL_METADATA == 1 ]]; then
  ln -s one "$SRC/link-rel"
  ln -s /some/absolute/elsewhere "$SRC/link-abs"
  ln -s missing "$SRC/link-dangling"
fi
touch -h -d '2020-02-02 02:02:02.123456789' "$SRC/one" "$SRC/many" 2> /dev/null
SRC_FILES=$(find "$SRC" -type f | wc -l)
note "source tree: $SRC_FILES files, $(du -sh "$SRC" | cut -f1)"

# ---- T1: first copy, verified --------------------------------------------------
em t1-sync sync -q --no-owner --require-checksum --journal "$WORK/t1.db" "$SRC" "$RUN/tree"
if [[ $rc == 0 ]]; then ok "T1 initial sync with --require-checksum"
else bad "T1 initial sync (exit $rc, see $LOG/t1-sync.out)"; fi
copied=$(count t1-sync 'files: [0-9]+ copied')
[[ $copied == "$SRC_FILES" ]] && ok "T1 copied all $SRC_FILES files" \
  || bad "T1 copied $copied of $SRC_FILES files"

# ---- T0: the target's self-test (in the run directory that T1 created) -----------
em t0-selftest selftest --no-owner "$RUN"
if [[ $rc == 0 ]]; then ok "T0 selftest"; else bad "T0 selftest (exit $rc)"; fi
sed 's/^/      /' "$LOG/t0-selftest.out" | tee -a "$LOG/results.txt"

# ---- T2: a rerun copies nothing -------------------------------------------------
em t2-rerun sync -q --no-owner "$SRC" "$RUN/tree"
copied=$(count t2-rerun 'files: [0-9]+ copied')
fixed=$(count t2-rerun 'metadata fixed: [0-9]+')
if [[ $rc == 0 && $copied == 0 && $fixed == 0 ]]; then ok "T2 rerun changes nothing"
else bad "T2 rerun: exit $rc, $copied copied, $fixed metadata fixed"; fi

# ---- T3: round trip back to local -------------------------------------------------
em t3-back sync -q --no-owner --require-checksum "$RUN/tree" "$WORK/back"
[[ $rc == 0 ]] && ok "T3 copy back with --require-checksum" || bad "T3 copy back (exit $rc)"
same_tree "$SRC" "$WORK/back" "T3-roundtrip"

# ---- T4: changes propagate, --delete -----------------------------------------------
chmod u+w "$SRC/modes/rodir"
# Same size, new content, new mtime.
head -c 1 /dev/urandom > "$SRC/one"; touch -d '2021-03-03 03:03:03.5' "$SRC/one"
mkfile "$SRC/multi" $((5 * CHUNK + 12345))
chmod 0640 "$SRC/modes/exec"
rm "$SRC/many/f7" "$SRC/deep/a/b/c/d/e/f/g/h/leaf"
rmdir "$SRC/deep/a/b/c/d/e/f/g/h"
[[ $FULL_METADATA == 1 ]] && ln -sfn chunk "$SRC/link-rel"
chmod 0555 "$SRC/modes/rodir"
em t4-sync sync -q --no-owner --require-checksum --delete "$SRC" "$RUN/tree"
deleted=$(count t4-sync 'deleted: [0-9]+')
[[ $rc == 0 ]] && ok "T4 sync of changes with --delete ($deleted deleted)" \
  || bad "T4 sync of changes (exit $rc)"
chmod -R u+w "$WORK/back"; rm -rf "$WORK/back"
em t4-back sync -q --no-owner "$RUN/tree" "$WORK/back"
if [[ $FULL_METADATA == 1 ]]; then
  same_tree "$SRC" "$WORK/back" "T4-changes"
else
  # Without mtimes on the target, same-size changes (one, multi) are missed.
  stale=$(diff -rq "$SRC" "$WORK/back" 2>&1 | grep -c differ)
  if diff -rq "$SRC" "$WORK/back" 2>&1 | grep differ | grep -qvE '/(one|multi) and '; then
    bad "T4 files other than the same-size changes differ (see diff -r $SRC $WORK/back)"
  elif [[ $stale -gt 0 ]]; then
    finding "T4 review finding 6: $stale files changed at the same size were not updated (no mtimes on the target)"
  else
    ok "T4-changes: trees identical"
  fi
fi

# ---- T5: review finding 1, a target that contains the source ------------------------
# Dry run only: nothing is changed.
em t5-overlap sync -q --no-owner --delete --dry-run "$RUN/tree/many" "$RUN/tree"
would=$(count t5-overlap 'would be deleted: [0-9]+')
if [[ ${would:-0} -gt 0 ]]; then
  finding "T5 target is the parent of the source: a real --delete run would delete $would entries"
else
  ok "T5 overlapping source and target refused or harmless (exit $rc)"
fi

# ---- T6: write windows, review finding 3 ------------------------------------------
mkdir -p "$WORK/win-src"
for i in 1 2 3; do mkfile "$WORK/win-src/w$i" $((3 * CHUNK + i * 4097)); done
for w in 1 4 16; do
  em "t6-w$w" sync -q --no-owner --require-checksum --write-window "$w" \
    "$WORK/win-src" "$RUN/window-$w"
  em "t6-w$w-back" sync -q --no-owner --require-checksum "$RUN/window-$w" "$WORK/win-back-$w"
  if [[ $rc == 0 ]] && diff -r "$WORK/win-src" "$WORK/win-back-$w" > /dev/null; then
    ok "T6 write window $w: data verified"
  else
    bad "T6 write window $w"
  fi
done
note "T6 checksums cover the data, not erasure-code parity. Whether the target"
note "   directory is erasure coded, and the parity check, need the eos CLI or an"
note "   admin; see the notes printed at the end."

# ---- T7/T8: interruption by SIGINT and SIGKILL --------------------------------------
BIG="$WORK/big"
nbig=8
for i in $(seq 1 $nbig); do mkfile "$BIG/b$i" $((BIG_MB * 1024 * 1024 / nbig)); done
interrupt() {  # interrupt NAME SIGNAL; sets $rc and $finished
  local name=$1 sig=$2
  "$BIN" sync -q --no-owner --journal "$WORK/$name.db" "$BIG" "$RUN/$name" \
    > "$LOG/$name.out" 2> "$LOG/$name.err" &
  local pid=$!
  # Wait until some data has gone out, but not all of it.
  sleep "${INTERRUPT_AFTER:-4}"
  finished=0
  kill -0 "$pid" 2> /dev/null || finished=1
  kill "-$sig" "$pid" 2> /dev/null
  wait "$pid"; rc=$?
  irc=$rc
  if [[ $finished == 1 ]]; then
    note "$name: the run ended before SIG$sig (exit $rc); raise BIG_MB or lower INTERRUPT_AFTER"
  else
    note "$name: interrupted with SIG$sig, exit $rc"
  fi
  # Whatever is visible under a real name must be a complete, correct file.
  em "$name-peek" sync -q --no-owner "$RUN/$name" "$WORK/$name-peek"
  local partial=0 present=0
  for f in "$WORK/$name-peek"/*; do
    [[ -e $f ]] || continue
    present=$((present + 1))
    cmp -s "$f" "$BIG/$(basename "$f")" || partial=$((partial + 1))
  done
  if [[ $partial == 0 ]]; then ok "$name: $present files visible, none partial"
  else bad "$name: $partial of $present visible files are partial or wrong"; fi
  rm -rf "$WORK/$name-peek"
  rc=$irc
}
interrupt t7-sigint INT
if [[ $finished == 0 ]]; then
  [[ $rc == 130 ]] && ok "T7 exit status 130 on SIGINT" || bad "T7 exit status $rc on SIGINT"
fi
em t7-resume sync -q --no-owner --journal "$WORK/t7-sigint.db" --resume "$BIG" "$RUN/t7-sigint"
em t7-back sync -q --no-owner --require-checksum "$RUN/t7-sigint" "$WORK/t7-back"
[[ $rc == 0 ]] && diff -r "$BIG" "$WORK/t7-back" > /dev/null \
  && ok "T7 --resume completes the tree" || bad "T7 --resume (exit $rc)"
rm -rf "$WORK/t7-back"

interrupt t8-sigkill KILL
em t8-rerun sync -q --no-owner --journal "$WORK/t8-sigkill.db" "$BIG" "$RUN/t8-sigkill"
stale=$(count t8-rerun 'stale temporaries: [0-9]+')
note "T8 temporaries left by the killed run, counted by the rerun: ${stale:-?} (only counted after 1 h)"
em t8-back sync -q --no-owner --require-checksum "$RUN/t8-sigkill" "$WORK/t8-back"
[[ $rc == 0 ]] && diff -r "$BIG" "$WORK/t8-back" > /dev/null \
  && ok "T8 a rerun after SIGKILL completes the tree" || bad "T8 rerun after SIGKILL (exit $rc)"
rm -rf "$WORK/t8-back" "$BIG"

# ---- T9: a name with '?' ---------------------------------------------------------
# EOS (and local targets) take it, as EOS paths are sent percent-encoded;
# plain XRootD cannot address it, which must fail that file alone.
mkdir -p "$WORK/qsrc"; mkfile "$WORK/qsrc/ok" 10; mkfile "$WORK/qsrc/what?" 10
em t9-qmark sync -q --no-owner "$WORK/qsrc" "$RUN/qmark"
failures=$(count t9-qmark 'failures: [0-9]+')
if [[ $FULL_METADATA == 1 ]]; then
  em t9-back sync -q --no-owner "$RUN/qmark" "$WORK/qback"
  [[ $rc == 0 ]] && cmp -s "$WORK/qsrc/what?" "$WORK/qback/what?" \
    && ok "T9 a name with '?' is copied and read back" \
    || bad "T9 a name with '?' (exit $rc, $failures failures)"
else
  [[ $rc == 1 && $failures == 1 ]] && ok "T9 a name with '?' fails alone, exit 1" \
    || bad "T9 name with '?': exit $rc, $failures failures"
fi

# ---- T10: a source file that changes while it is copied ---------------------------
mkfile "$WORK/chg/moving" $((256 * 1024 * 1024))
( for i in $(seq 1 40); do
    dd if=/dev/urandom of="$WORK/chg/moving" bs=1M count=1 seek=$((RANDOM % 256)) \
      conv=notrunc status=none; sleep 0.2
  done ) &
writer=$!
em t10-changing sync -q --no-owner "$WORK/chg" "$RUN/changing"
wait $writer
em t10-back sync -q --no-owner "$RUN/changing" "$WORK/chg-back"
if [[ -e $WORK/chg-back/moving ]]; then
  # A committed copy must be the source as it is now (the rerun below fixes
  # any copy that was refused).
  em t10-rerun sync -q --no-owner "$WORK/chg" "$RUN/changing"
  rm -rf "$WORK/chg-back"; em t10-back2 sync -q --no-owner "$RUN/changing" "$WORK/chg-back"
  cmp -s "$WORK/chg/moving" "$WORK/chg-back/moving" \
    && ok "T10 a file changed during the copy ends up correct after a rerun" \
    || bad "T10 the copy of a changing file differs from the source after a rerun"
else
  note "T10 the changing file was not committed (retries exhausted), as designed"
fi

# ---- summary ------------------------------------------------------------------------
say ""
say "passed: $pass, failed: $fail, review findings reproduced: $findings"
say "logs: $LOG"
cat <<EOF | tee -a "$LOG/results.txt"

Not testable as an ordinary user (needs the eos CLI with admin rights):
  - Layout of the target:   eos attr ls $(echo "$RUN" | sed 's|^root://[^/]*/||')
  - EC parity (finding 3):  for an erasure-coded directory, eos fsck / eos file
    verify, or reading the files back with one filesystem set offline.
  - Owners (--no-owner was used throughout) and the root/sudoer tests of
    tests/integration/test_eos.cc.
EOF
say ""
say "The test files stay on the target; remove them with:"
say "  eos rm -r $(echo "$RUN" | sed 's|^root://[^/]*/||')    (or xrdfs/FUSE)"
exit $((fail > 0))
