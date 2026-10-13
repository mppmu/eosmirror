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
- Extra entries on the target are only deleted with an explicit option
  (`--delete`), which behaves like rsync's: it deletes in directories that
  are empty on the source too, and takes an optional cap on the number of
  deletions (`--max-delete`). A run whose source is empty as a whole stops
  instead of emptying the target; `--keep-emptied` also keeps the entries
  of directories below that are empty on the source (as an unmounted
  automount looks). Nothing is deleted in directories where a listing had
  to skip entries, nor where the source, asked before deleting, has an
  entry its listing lacked; those deletions, and the ones beyond the cap,
  are failures that `--retry-failed` takes up again. A source and target
  that overlap (one inside the other) are refused.
- Entries that a listing of the source has to skip (names that cannot be
  used as paths, lines of a server's listing that cannot be told apart)
  are not copied, which is a failure of their directory, as with rsync.
- Files and directories that disappear from the source during a run are
  counted as vanished, not as failures; the next run finds them gone.
- Errors are retried with backoff; what still fails is recorded in the
  journal and reported, and the exit status says so. When everything fails
  (an endpoint has gone away), the run stops after 1000 failures or vanished
  entries in a row (`--max-consecutive-failures`).

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
uploads); `eos://` names an EOS instance explicitly. Paths below `/eos`
are on the instance of `--mgm` or `EOS_MGM_URL`, as for the `eos` client.
A run logs both endpoints as resolved (server, real path, and where the MGM
came from). Authentication is the
XRootD client's (`XrdSecPROTOCOL`, `XrdSecSSSKT`, Kerberos tickets).
eosmirror shows URLs without passwords and opaque parameters, but XrdCl's
own log (`XRD_LOGLEVEL`) shows passwords given in URLs: give none there.

For example, a first run with a journal, then a rerun of what failed:

```
eosmirror sync --journal migration.db --progress 60 /data/project root://eos.example.org//eos/project
eosmirror sync --journal migration.db --retry-failed /data/project root://eos.example.org//eos/project
```

Setting owners on EOS needs an identity that EOS treats as root (a sudoer cannot);
the run refuses to start otherwise unless `--no-owner` is given. Plain
XRootD servers store no mtimes and have no symlinks: a run against one
needs `--size-only`, which compares files by size alone (as with rsync,
it does so on any target, and sets the mtimes of files that match where
the target stores them), and symlinks are skipped and counted.

Each file is copied in chunks of `--buffer-size` (8 MiB) with up to 4 reads
and 4 writes in flight (`--read-window`, `--write-window`). Writes go out in
order, each where the previous one ended, which erasure-coded EOS files
take only under a condition (see below). Every thread talks to XRootD and
EOS servers over connections of its own, since a server handles the
requests of a connection one at a time (`--shared-connection` shares one
per server). Files read from EOS or from an XRootD server that computes
checksums are verified against the stored checksum when the target
computes none.
Files read from EOS take their metadata and checksum from the directory
listing, so the MGM sees one open per file, as with xrdcp; a file that
changed since the listing is stated before and after reading.

Up to 64 copies run at once (`--max-transfers`). A run starts with 4
(`--min-transfers`) and raises the number every 10 s while the throughput
improves, takes a raise back when the throughput drops after it, and
lowers the number when the target fails operations; `--transfers N` runs
exactly N. A copy holds up to both windows plus one buffer, so 64 copies
hold up to 4.5 GiB of buffers at the defaults (2.5 GiB between EOS and a
local file system).

On a terminal, `sync` shows a live display: counters, the write rate, bars
for the transfer slots in use and the backlog, and one bar per file being
copied. A "listing-bound" count there and in the summary says how often
transfers had nothing to copy while every directory worker was busy: more
`--checkers` may help. `--progress SEC` prints plain progress lines instead (for logs) and
`-q` turns both off.

`eosmirror sync --help` lists the options: dry runs, deletion of extra
entries with a cap, parallelism (`--min-transfers`, `--max-transfers`,
`--transfers`), retries, resuming an interrupted run,
sharding a tree over several hosts (`--shard K/N`) and symlink rewriting.
The exit status is 0 when everything succeeded, 1 when some entries failed
after retries, 2 for usage errors, 3 when the run could not start (or found
the source empty but not the target, with `--delete`, or source and target
overlapping) or its journal failed, 4 when it stopped
after too many failures in a row (try again later with `--resume`) and 130
when interrupted.

## Pipelined writes and erasure coding

Each file is written with up to `--write-window` writes of `--buffer-size`
in flight (4 × 8 MiB by default), in offset order over one connection,
each starting where the previous one ended. xrdcp and `eos rclone` (which
copies through XrdCl's copy process) do the same, with 4 writes of 8 MiB.

For erasure-coded (RAIN) directories on EOS, this is safe only while the
storage nodes (FSTs) run with EOS's default `xrootd.async off`, which EOS's
packaged FST configuration sets. XrdCl asks for asynchronous I/O on every
open, so an FST with `xrootd.async on` may execute the writes of a file out
of order, and EOS up to 5.5.2 then stores zero parity: the file reads
correctly until one of its stripes is lost. XrdCl never sends writes over
substreams, so `XRD_SUBSTREAMSPERCHANNEL` does not matter. A client cannot
ask an FST for the setting: when in doubt, use `--write-window 1`, which
sends each write once the previous one has completed and is slower over
long distances. Runs with an EOS target and a larger window say so at the
start.

## Known limitations

- Plain XRootD servers cannot show symlinks: their listings follow them,
  and removing one removes the file it points to (XRootD's
  `XrdOssUnlink.cc`), so that deleting a directory through a symlink
  empties the directory it points to. `--delete` against a plain XRootD
  target is therefore refused unless `--target-without-symlinks` states
  that it has none. Local and EOS targets show symlinks and remove them
  as such.

## Building

Requires a C++20 compiler (gcc 11 or newer), CMake 3.20 or newer, SQLite 3
and zlib. The XRootD client library (XrdCl, XRootD 5.6 or newer) is needed
for XRootD and EOS endpoints.

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
CERN's public EOS image; CI does so nightly. The XRootD tests also run
against an xrootd installed on the host, as in CI with Ubuntu's
`xrootd-server`:

```
EOSMIRROR_XROOTD_LOCAL=1 tests/integration/xrootd-server.sh start
EOSMIRROR_XROOTD_URL=root://localhost:1094//data ctest --test-dir build -R integration
EOSMIRROR_XROOTD_LOCAL=1 tests/integration/xrootd-server.sh stop
```

Two longer tests (about a minute each) run only when asked for, as in
CI's nightly job: `ctest --test-dir build -C long -L long`. One kills
syncs of a local tree at random points with SIGKILL and checks the copy
once a run completes; the other changes random trees at random, syncs them
with injected faults and compares the result in content and metadata.
Both print their seed; `EOSMIRROR_SEED=<seed>` repeats a failing one.

## Packaging

`packaging/eosmirror.spec` builds an RPM (`rpmbuild -ta eosmirror-VERSION.tar.gz`)
and `debian/` a Debian package (`dpkg-buildpackage -us -uc -b`).
`scripts/build-packages.sh [--output=DIR] [--image=IMAGE...] [PLATFORM...]`
builds both from the working tree: in the build containers, an RPM for EL9
and .debs for Ubuntu 22.04 and 24.04, and with `--image` in fresh containers
from stock Debian, Ubuntu, EL, Fedora or openSUSE images, against the
distribution's XRootD (EPEL's on EL). It installs each of the latter in a
second fresh container and runs the self-test there:

```
scripts/build-packages.sh --image=docker.io/library/debian:13 \
  --image=docker.io/library/almalinux:9 --image=registry.fedoraproject.org/fedora:44
```

Debian 12, Ubuntu 22.04 and openSUSE Leap 16.0 ship no XRootD 5.6 or newer.

The XRootD client library comes from CERN's eos-xrootd (under
`/opt/eos/xrootd`, the XRootD that EOS is built against) or from the
distribution. The RPM uses eos-xrootd unless built `--without eos_xrootd`,
the Debian build uses it when it is installed.

## License

GPL-3.0-or-later, see [LICENSE](LICENSE). The bundled test framework
[doctest](https://github.com/doctest/doctest) is MIT licensed.
