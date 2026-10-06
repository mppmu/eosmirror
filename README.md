# eosmirror

One-way replication of large file trees between POSIX file systems, XRootD
servers and [EOS](https://eos-web.web.cern.ch/) instances.

eosmirror copies a tree from a source to a target so that the target ends up
with the same files, symlinks, directories, owners, modes and modification
times. It is made for very large trees over high-latency links: it keeps many
transfers and metadata operations in flight, works directory by directory with
bounded memory, never stops on single errors, and keeps a journal so that a
later run retries exactly what failed.

Status: work in progress. Local file systems, plain XRootD servers and EOS
instances work as source and target; EOS to EOS copies stream through the
client for now. See [docs/design.md](docs/design.md).

## Behavior in short

- A file is copied when it is missing on the target or differs in size or
  modification time. Otherwise only owner, group, mode and (for directories
  and symlinks) the modification time are fixed.
- Copies are atomic (temporary name plus rename, or EOS atomic uploads), set
  the source's modification time, and verify a checksum end to end where the
  endpoint supports it.
- Symlinks are recreated as symlinks and never followed. Hard links are copied
  as separate files. Special files are skipped and reported.
- Extra files on the target are only deleted with an explicit option, with a
  dry run and a cap on the number of deletions.
- Errors are retried with backoff; what still fails is recorded in the
  journal and reported, and the exit status says so.

Extended attributes and ACLs are not replicated.

## Compared with xrdcp and eos rclone

`xrdcp -r --parallel N` copies a tree once with N transfers at a time. It
copies the files that symlinks point to rather than the links, keeps no
mtimes, owners or modes, and does not compare with an existing target: a
rerun either fails on existing files or, with `--force`, writes them all
again. Checksums are verified on request, and it can use third-party copy.

`eos rclone` copies a tree between a local path and an EOS instance, or
within one. It lists the whole tree before copying, copies a file when the
source is newer, keeps symlinks, keeps mtimes on EOS but not on local
targets, creates directories with a fixed mode, sets no owners, verifies no
checksums, copies with a fixed parallelism, and its exit status does not
reflect failed copies.

eosmirror works between any two of local file systems, XRootD servers and
EOS instances, directory by directory with bounded memory, copies a file
when size or mtime differ, keeps symlinks, owners, modes and mtimes, writes
atomically with checksums verified end to end where the endpoint computes
them, retries and journals failures, and adapts its concurrency to the
target. EOS to EOS copies stream through the client; third-party copy is
planned.

## Usage

```
eosmirror sync [options] SOURCE TARGET
eosmirror selftest [--no-owner] TARGET
eosmirror failures JOURNAL
```

`selftest` checks, in a temporary directory under the target, that
everything a run needs works there (creating files with verified checksums,
modes, owners, mtimes, symlinks, listings, removal) and says what the target
cannot do.

Endpoints are local paths or XRootD URLs. An XRootD URL that answers to
EOS commands is treated as EOS (symlinks, owners, nanosecond mtimes, atomic
uploads); `eos://` names an EOS instance explicitly. Authentication is the
XRootD client's (`XrdSecPROTOCOL`, `XrdSecSSSKT`, Kerberos tickets).

For example, a first run with a journal, then a rerun of what failed:

```
eosmirror sync --journal migration.db --progress 60 /data/project root://eos.example.org//eos/project
eosmirror sync --journal migration.db --retry-failed /data/project root://eos.example.org//eos/project
```

Setting owners on EOS needs an identity that EOS treats as root or sudoer;
the run refuses to start otherwise unless `--no-owner` is given. Plain
XRootD servers store no mtimes and have no symlinks, so against them files
are compared by size only and symlinks are skipped (both are reported).

Each file is copied in chunks of `--buffer-size` (8 MiB) with up to 4 reads
and 4 writes in flight (`--read-window`, `--write-window`), over one
connection per server. Writes go out in order, each where the previous one
ended. This is safe for erasure-coded EOS files only while the storage
nodes (FSTs) keep EOS's default `xrootd.async off`; `--write-window 1` opts
out. Files read from EOS or from an XRootD server that computes checksums
are verified against the stored checksum when the target computes none.

On a terminal, `sync` shows a live display: counters, the write rate, bars
for the transfer slots in use and the backlog, and one bar per file being
copied. `--progress SEC` prints plain progress lines instead (for logs) and
`-q` turns both off.

`eosmirror sync --help` lists the options: dry runs, deletion of extra
entries with a cap, worker counts, retries, resuming an interrupted run,
sharding a tree over several hosts (`--shard K/N`) and symlink rewriting.
The exit status is 0 when everything succeeded, 1 when some entries failed
after retries, 2 for usage errors, 3 when the run could not start and 130
when interrupted.

## Building

Requires a C++20 compiler (gcc 11 or newer), CMake 3.20 or newer, SQLite 3
and zlib. The XRootD client library (XrdCl) is needed for XRootD and EOS
endpoints.

```
cmake -S . -B build
cmake --build build
ctest --test-dir build
```

`containers/` has container recipes for the supported platforms (EL9,
Ubuntu 22.04 and 24.04) and `scripts/check.sh` builds and tests in one of
them, optionally under AddressSanitizer or ThreadSanitizer.
`scripts/integration-test.sh` runs the integration tests against a plain
xrootd server and a single-host EOS instance started in containers from
CERN's public EOS image.

## Packaging

`packaging/eosmirror.spec` builds an RPM (`rpmbuild -ta eosmirror-VERSION.tar.gz`)
and `debian/` a Debian package (`dpkg-buildpackage -us -uc -b`).
`scripts/build-packages.sh [--output=DIR] [PLATFORM...]` builds both from
the working tree in the build containers, an RPM for EL9 and .debs for
Ubuntu 22.04 and 24.04.

The XRootD client library comes from CERN's eos-xrootd (under
`/opt/eos/xrootd`, the XRootD that EOS is built against) or from the
distribution. The RPM uses eos-xrootd unless built `--without eos_xrootd`,
the Debian build uses it when it is installed.

## License

GPL-3.0-or-later, see [LICENSE](LICENSE). The bundled test framework
[doctest](https://github.com/doctest/doctest) is MIT licensed.
