#!/bin/bash
# Build the binary packages in the build containers: an RPM on EL (rpmbuild
# -ta) and a .deb on Ubuntu (dpkg-buildpackage), both from a release tarball
# of the working tree. Runs rpmlint and lintian on the results.
#
# Usage: scripts/build-packages.sh [--output=DIR] [platform...]   (default: all)
#   --output  where the tarball and, per platform, the packages go
#             (default: <root>/packages)
# Env:   EOSMIRROR_BUILD_ROOT as for in-container.sh. The packages are built
#        in <root>/<platform>/packages.
set -euo pipefail

here=$(dirname "$(readlink -f "$0")")
repo=$(readlink -f "$here/..")
build_root=${EOSMIRROR_BUILD_ROOT:-${TMPDIR:-/tmp}/eosmirror-build}
out=$build_root/packages

platforms=()
for arg; do
  case $arg in
    --output=*) out=${arg#--output=} ;;
    --*) echo "build-packages.sh: unknown option $arg" >&2; exit 2 ;;
    *) platforms+=("$arg") ;;
  esac
done
if [[ ${#platforms[@]} == 0 ]]; then
  for f in "$repo"/containers/Containerfile.*; do platforms+=("${f##*/Containerfile.}"); done
fi

# The package versions must follow the project's.
version=$(sed -n 's/^project(eosmirror VERSION \([0-9.]*\).*/\1/p' "$repo/CMakeLists.txt")
grep -q "^Version: *$version\$" "$repo/packaging/eosmirror.spec" \
  || { echo "build-packages.sh: packaging/eosmirror.spec is not at version $version" >&2; exit 1; }
[[ $(head -n 1 "$repo/debian/changelog") == "eosmirror ($version)"* ]] \
  || { echo "build-packages.sh: debian/changelog is not at version $version" >&2; exit 1; }

# The working tree, including uncommitted changes, as a release tarball.
name=eosmirror-$version
mkdir -p "$out"
tar -C "$repo" --exclude-vcs --exclude=./build --exclude='./build-*' \
  --owner=0 --group=0 --mode=u+rwX,go+rX,go-w --transform="s,^\.,$name," \
  -czf "$out/$name.tar.gz" .

for p in "${platforms[@]}"; do
  case $p in
    el*) build="
      rpmbuild -ta --define '_topdir /build/packages/rpm' $name.tar.gz
      mv rpm/RPMS/*/*.rpm rpm/SRPMS/*.rpm out/
      rpmlint out/*.rpm || true" ;;
    # dpkg-buildpackage writes the packages next to the source tree.
    ubuntu*|debian*) build="
      tar -C out -xzf $name.tar.gz
      (cd out/$name && dpkg-buildpackage -us -uc -b)
      rm -r out/$name
      lintian out/*.changes || true" ;;
    *) echo "build-packages.sh: no package format for platform $p" >&2; exit 2 ;;
  esac

  work=$build_root/$p/packages
  rm -rf "$work" "${out:?}/$p"
  mkdir -p "$work/out"
  cp "$out/$name.tar.gz" "$work/"
  "$here/in-container.sh" "$p" bash -ec "cd /build/packages; $build"
  cp -r "$work/out" "$out/$p"
  ls -l "$out/$p"
done
