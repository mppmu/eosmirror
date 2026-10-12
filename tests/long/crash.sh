#!/bin/bash
# Crash consistency: syncs of a generated local tree are killed with SIGKILL
# at random points several times and then run until one completes, with the
# journal, --resume and --delete as an operator would run them. The copy
# must then equal the source in content and metadata. Several rounds change
# the source in between (files rewritten with and without another size,
# deleted, added, given other modes, replaced by directories). The seed
# (printed; EOSMIRROR_SEED or the second argument) reproduces the trees and
# the changes, though not the contents of the files nor where in a run the
# kills hit, which depends on its speed.
#
# Usage: crash.sh PATH_TO_EOSMIRROR [SEED]
# Env:   EOSMIRROR_ROUNDS  rounds (default 60)
set -euo pipefail

bin=${1:?usage: crash.sh EOSMIRROR [SEED]}
seed=${2:-${EOSMIRROR_SEED:-$(($(date +%s) % 100000))}}
rounds=${EOSMIRROR_ROUNDS:-60}
echo "seed $seed"
# RANDOM is reseeded in subshells: it is used only in this shell.
RANDOM=$seed
work=$(mktemp -d "${TMPDIR:-/tmp}/eosmirror-crash-XXXXXX")
src=$work/src
dst=$work/dst
round=0
pid=

fail() {
  echo "FAIL (seed $seed, round $round, kept $work): $*" >&2
  exit 1
}

# A run still going when the test ends (killed by a timeout) goes too.
stop_run() {
  if [[ -n $pid && $(cat "/proc/$pid/comm" 2> /dev/null) == eosmirror ]]; then
    kill -9 "$pid" 2> /dev/null || true
  fi
}
trap stop_run EXIT

owner_opt=()
[[ $(id -u) == 0 ]] || owner_opt=(--no-owner)

# Sets when to a random time for touch -d.
random_time() { when="@$((1500000000 + RANDOM * 3000)).$((RANDOM % 1000))"; }

# A file of random content: empty, up to 20 kB, or up to 1 MiB.
new_file() {
  local size
  case $((RANDOM % 10)) in
    0) size=0 ;;
    1) size=$((RANDOM * 32)) ;;
    *) size=$((RANDOM % 20000)) ;;
  esac
  head -c "$size" /dev/urandom > "$1"
  local mode=644
  ((RANDOM % 3)) || mode=600
  chmod "$mode" "$1"
  random_time
  touch -d "$when" "$1"
}

# Three levels of directories with files and a symlink each.
for ((d = 0; d < 30; d++)); do
  dir=$src/a$((d % 3))/b$((d % 7))/c$d
  mkdir -p "$dir"
  for ((f = 0; f < 15 + RANDOM % 25; f++)); do new_file "$dir/f$f"; done
  ln -s f0 "$dir/link"
  random_time
  touch -h -d "$when" "$dir/link"
done

# Changes between rounds, of about a quarter of the files.
change_source() {
  local files dirs path i
  mapfile -t files < <(find "$src" -type f | sort)
  mapfile -t dirs < <(find "$src" -mindepth 3 -type d | sort)
  for ((i = 0; i < ${#files[@]} / 4; i++)); do
    path=${files[RANDOM % ${#files[@]}]}
    [[ -f $path ]] || continue
    case $((RANDOM % 6)) in
      0 | 1) new_file "$path" ;;
      2)  # the same size, other content and mtime
        head -c "$(stat -c %s "$path")" /dev/urandom > "$path"
        random_time
        touch -d "$when" "$path"
        ;;
      3) rm "$path" ;;
      4) chmod 640 "$path" ;;
      5)  # replaced by a directory
        rm "$path"
        mkdir "$path"
        new_file "$path/inner"
        ;;
    esac
  done
  for ((i = 0; i < 10; i++)); do
    new_file "${dirs[RANDOM % ${#dirs[@]}]}/n$round-$i"
  done
}

# The tree as find describes it, eosmirror's temporaries of killed runs
# aside.
describe() {
  (cd "$1" && find . -name '.*.eosmirror-*' -prune -o \
    \( -type f -printf '%P file %m %T@ %s\n' \) -o \
    \( -type d -printf '%P dir %m %T@\n' \) -o \
    \( -type l -printf '%P link %T@ %l\n' \) | sort)
}

args=(sync -q "${owner_opt[@]}" --journal "$work/journal.db" --resume --delete --delete-emptied
  --checkers 4 --transfers 8 --buffer-size 64k "$src" "$dst")
total_kills=0
# Kills fall within the time a run takes (ms): that of the last complete
# one, but at least a quarter of the longest, since a round changes a
# quarter of the files and the last complete run may only have finished
# what killed ones left.
duration=100
longest=0
for ((round = 0; round < rounds; round++)); do
  # Each round from a seed of its own: the draws of the runs before differ.
  RANDOM=$((seed * 1000 + round))
  ((round == 0)) || change_source
  # Directories get their mtimes once their changes are made.
  while read -r dir; do
    random_time
    touch -d "$when" "$dir"
  done < <(find "$src" -depth -type d)

  kills=0
  for ((attempt = 0; attempt < 30; attempt++)); do
    window=$((duration > longest / 4 ? duration : longest / 4))
    delay=$((1 + RANDOM % window))
    started=$(date +%s%N)
    "$bin" "${args[@]}" > "$work/run.out" 2>&1 &
    pid=$!
    if ((kills < 6)); then
      sleep "$((delay / 1000)).$(printf %03d $((delay % 1000)))"
      kill -9 "$pid" 2> /dev/null && kills=$((kills + 1))
    fi
    rc=0
    wait "$pid" 2> /dev/null || rc=$?
    if ((rc == 0)); then
      duration=$(( ($(date +%s%N) - started) / 1000000 + 1 ))
      ((duration > longest)) && longest=$duration
      break
    fi
    ((rc == 137)) || fail "a run exited with $rc: $(cat "$work/run.out")"
  done
  ((rc == 0)) || fail "no run completed"
  total_kills=$((total_kills + kills))

  diff <(describe "$src") <(describe "$dst") > "$work/meta.diff" \
    || fail "metadata differs: $(head -20 "$work/meta.diff")"
  diff -r --no-dereference -x '.*.eosmirror-*' "$src" "$dst" > "$work/content.diff" \
    || fail "content differs: $(head -20 "$work/content.diff")"
done
temporaries=$(find "$dst" -name '.*.eosmirror-*' | wc -l)
echo "$rounds rounds, $total_kills runs killed, $temporaries temporaries of killed runs left"
# Those show that kills hit copies under way.
((temporaries > 0)) || fail "no run was killed while it copied"

"$bin" sync "${owner_opt[@]}" "$src" "$dst" > "$work/rerun.out" 2> "$work/rerun.err" \
  || fail "a further run failed: $(cat "$work/rerun.err")"
grep -q 'files: 0 copied' "$work/rerun.out" && grep -q 'metadata fixed: 0' "$work/rerun.out" \
  || fail "a further run changed something: $(cat "$work/rerun.out")"

rm -rf "$work"
echo "crash consistency test passed"
