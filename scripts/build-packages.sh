#!/bin/bash
# Build the binary packages, RPMs (rpmbuild -ta) or .debs (dpkg-buildpackage),
# from a release tarball of the working tree, in the build containers of the
# given platforms or in fresh containers from stock distribution images, and
# run rpmlint and lintian on the results.
#
# Usage: scripts/build-packages.sh [--output=DIR] [--image=IMAGE...] [platform...]
#        (default: all platforms)
#   --output  where the tarball and, per platform or image, the packages go
#             (default: <root>/packages)
#   --image   build in a container from IMAGE (Debian, Ubuntu, EL, Fedora or
#             openSUSE, e.g. docker.io/library/debian:13), with the build
#             dependencies from the distribution's repositories (and EPEL on
#             EL), then install the package in a second one and run the
#             self-test. The packages and the logs go to DIR/<IMAGE with
#             characters other than [A-Za-z0-9._-] replaced by _>.
# Env:   EOSMIRROR_BUILD_ROOT as for in-container.sh. The packages are built
#        in <root>/<platform>/packages.
set -euo pipefail

here=$(dirname "$(readlink -f "$0")")
repo=$(readlink -f "$here/..")
build_root=${EOSMIRROR_BUILD_ROOT:-${TMPDIR:-/tmp}/eosmirror-build}
out=$build_root/packages

platforms=()
images=()
for arg; do
  case $arg in
    --output=*) out=${arg#--output=} ;;
    --image=?*) images+=("${arg#--image=}") ;;
    --*) echo "build-packages.sh: unknown option $arg" >&2; exit 2 ;;
    *) platforms+=("$arg") ;;
  esac
done
if [[ ${#platforms[@]} == 0 && ${#images[@]} == 0 ]]; then
  for f in "$repo"/containers/Containerfile.*; do platforms+=("${f##*/Containerfile.}"); done
fi

# The package versions must match the project's.
version=$(sed -n 's/^project(eosmirror VERSION \([0-9.]*\).*/\1/p' "$repo/CMakeLists.txt")
grep -q "^Version: *$version\$" "$repo/packaging/eosmirror.spec" \
  || { echo "build-packages.sh: packaging/eosmirror.spec is not at version $version" >&2; exit 1; }
[[ $(head -n 1 "$repo/debian/changelog") == "eosmirror ($version)"* ]] \
  || { echo "build-packages.sh: debian/changelog is not at version $version" >&2; exit 1; }

# The working tree, including uncommitted changes, as a release tarball.
name=eosmirror-$version
mkdir -p "$out"
out=$(readlink -f "$out")
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

# What runs in the containers from stock images, which have the tarball in /in
# and the output directory at /out.

# The distribution's package family: deb, el, fedora or suse.
image_family() {
  . /etc/os-release
  case " $ID ${ID_LIKE-} " in
    *" debian "* | *" ubuntu "*) echo deb ;;
    *" suse "*) echo suse ;;
    " fedora "*) echo fedora ;;
    *" rhel "* | *" fedora "*) echo el ;;
    *) echo "build-packages.sh: no package format for $PRETTY_NAME" >&2; return 1 ;;
  esac
}

pkg_install() {
  case $family in
    deb) apt-get -y install --no-install-recommends "$@" ;;
    el | fedora) dnf -y install --setopt=install_weak_deps=False "$@" ;;
    suse) zypper -n install --no-recommends "$@" ;;
  esac
}

# Builds the packages from /in/NAME.tar.gz into /out, with the build
# dependencies the package metadata names. The RPMs use the distribution's
# XRootD rather than eos-xrootd.
image_build() {
  local name=$1 spec=$1/packaging/eosmirror.spec
  family=$(image_family)
  mkdir /build
  cd /build
  case $family in
    deb) apt-get update ;;
    el) pkg_install epel-release dnf-plugins-core; crb enable; pkg_install rpm-build ;;
    fedora) pkg_install rpm-build 'dnf5-command(builddep)' ;;
    suse) pkg_install rpm-build ;;
  esac
  tar -xzf "/in/$name.tar.gz"
  case $family in
    deb) apt-get -y build-dep --no-install-recommends "./$name" ;;
    el | fedora) dnf -y builddep --setopt=install_weak_deps=False -D '_without_eos_xrootd 1' "$spec" ;;
    suse) mapfile -t deps < <(rpmspec -q --buildrequires "$spec"); pkg_install "${deps[@]}" ;;
  esac
  if [[ $family == deb ]]; then
    # dpkg-buildpackage writes the packages next to the source tree.
    (cd "$name" && dpkg-buildpackage -us -uc -b)
    find . -maxdepth 1 -type f -exec mv -t /out {} +
    { pkg_install lintian && lintian /out/*.changes; } || true
  else
    rpmbuild -ta --without eos_xrootd --define '_topdir /build/rpm' "/in/$name.tar.gz"
    mv rpm/RPMS/*/*.rpm rpm/SRPMS/*.rpm /out/
    { pkg_install rpmlint && rpmlint /out/*.rpm; } || true
  fi
}

# Installs the package from /out, with the dependencies from the
# distribution's repositories (and EPEL on EL), and runs the self-test.
image_install() {
  local rpm
  family=$(image_family)
  rpm=$(echo /out/eosmirror-[0-9]*."$(uname -m)".rpm)
  case $family in
    deb) apt-get update; pkg_install /out/eosmirror_*.deb ;;
    el) pkg_install epel-release; pkg_install "$rpm" ;;
    fedora) pkg_install "$rpm" ;;
    suse) pkg_install --allow-unsigned-rpm "$rpm" ;;
  esac
  eosmirror --version
  mkdir -p /tmp/x
  eosmirror selftest --no-owner /tmp/x
}

# Runs COMMAND, one of the above, in a fresh container from $image, with the
# output in $dir/STEP.log.
in_image() {  # STEP COMMAND PODMAN_OPTION...
  local step=$1 command=$2
  shift 2
  podman run --rm -e DEBIAN_FRONTEND=noninteractive "$@" "$image" bash -eo pipefail -c \
    "$(declare -f image_family pkg_install image_build image_install); $command" 2>&1 \
    | tee "$dir/$step.log" \
    || { echo "build-packages.sh: $step in $image failed, see $dir/$step.log" >&2; exit 1; }
}

for image in "${images[@]}"; do
  dir=$out/${image//[^A-Za-z0-9._-]/_}
  rm -rf "$dir"
  mkdir -p "$dir"
  in_image build "image_build $name" -v "$out/$name.tar.gz:/in/$name.tar.gz:ro" -v "$dir:/out"
  in_image install image_install -v "$dir:/out:ro"
  ls -l "$dir"
done
