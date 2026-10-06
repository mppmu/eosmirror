# SPDX-License-Identifier: GPL-3.0-or-later
#
# From a release tarball:  rpmbuild -ta eosmirror-0.1.0.tar.gz
# From the source tree:    rpmbuild -bb --build-in-place packaging/eosmirror.spec
#
# The XRootD client library is CERN's eos-xrootd under /opt/eos/xrootd (the
# version EOS is built against, the default on EL) or, --without eos_xrootd,
# the distribution's (EPEL, openSUSE).
%if 0%{?suse_version}
%bcond_with eos_xrootd
%else
%bcond_without eos_xrootd
%endif

Name:           eosmirror
Version:        0.1.0
Release:        1%{?dist}
Summary:        One-way replication of large file trees between file systems, XRootD and EOS
License:        GPL-3.0-or-later
# URL:          the GitHub repository, once public
Source0:        %{name}-%{version}.tar.gz

BuildRequires:  cmake >= 3.20
BuildRequires:  make
BuildRequires:  pkgconfig(sqlite3)
%if 0%{?suse_version} && 0%{?suse_version} < 1600
# The default compiler of Leap 15 is gcc 7.
BuildRequires:  gcc13-c++
%else
BuildRequires:  gcc-c++ >= 11
%endif

%if %{with eos_xrootd}
BuildRequires:  eos-xrootd
# eos-xrootd provides no sonames, so it is required by name.
Requires:       eos-xrootd%{?_isa}
%global __requires_exclude ^libXrd
%else
BuildRequires:  xrootd-client-devel
# The security plugins (sss, krb5) are loaded at run time.
Requires:       xrootd-libs%{?_isa}
%endif

%description
eosmirror copies a file tree from a source to a target so that the target
ends up with the same files, symlinks, directories, owners, modes and
modification times. Sources and targets are local file systems, XRootD servers
and EOS instances. It is made for very large trees over high-latency links:
it keeps many transfers in flight, works directory by directory with bounded
memory, and keeps a journal so that a later run retries exactly what failed.

%prep
%autosetup

%build
%if 0%{?suse_version} && 0%{?suse_version} < 1600
export CXX=g++-13
%endif
%cmake -DEOSMIRROR_WERROR=OFF -DEOSMIRROR_XROOTD_PREFIX=%{?with_eos_xrootd:/opt/eos/xrootd}
%cmake_build

%install
%cmake_install

%check
# The integration test needs servers, see scripts/integration-test.sh.
%ctest --exclude-regex '^integration$'

%files
%license LICENSE
%doc README.md docs/design.md AI_POLICY.md
%{_bindir}/eosmirror

%changelog
* Tue Oct 06 2026 eosmirror maintainers <eosmirror@localhost> - 0.1.0-1
- Initial release
