#!/bin/bash
# Creates a tree of dummy data for scale tests: DIRS directories, each with
# FILES files of SIZE bytes (plus one big file per 50 directories), nested
# three levels deep, with a symlink per directory. Content is random, so
# that checksums are meaningful.
#
# Usage: make-tree.sh ROOT DIRS FILES SIZE [BIGSIZE]
#   e.g. make-tree.sh /tmp/eosmirror-src 1000 100 4k 1G
set -eu

root=${1:?usage: make-tree.sh ROOT DIRS FILES SIZE [BIGSIZE]}
dirs=${2:?}
files=${3:?}
size=${4:?}
bigsize=${5:-256M}

to_bytes() {
  local v=$1
  case $v in
    *k) echo $(( ${v%k} * 1024 )) ;;
    *M) echo $(( ${v%M} * 1024 * 1024 )) ;;
    *G) echo $(( ${v%G} * 1024 * 1024 * 1024 )) ;;
    *) echo "$v" ;;
  esac
}
bytes=$(to_bytes "$size")
bigbytes=$(to_bytes "$bigsize")

mkdir -p "$root"
for ((d = 0; d < dirs; d++)); do
  dir=$root/level1-$((d / 100))/level2-$((d / 10))/dir-$d
  mkdir -p "$dir"
  for ((f = 0; f < files; f++)); do
    head -c "$bytes" /dev/urandom > "$dir/file-$f.dat"
  done
  ln -sf "file-0.dat" "$dir/link"
  if ((d % 50 == 0)); then
    head -c "$bigbytes" /dev/urandom > "$dir/big.dat"
  fi
done
echo "created $dirs directories with $files files of $size each under $root"
