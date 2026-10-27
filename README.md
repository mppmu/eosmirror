# eosmirror

One-way replication of large file trees between POSIX file systems, XRootD
servers and [EOS](https://eos-web.web.cern.ch/) instances.

eosmirror copies a tree from a source to a target so that the target ends up
with the same files, symlinks, directories, owners, modes and modification
times. It is made for very large trees over high-latency links: it keeps many
transfers and metadata operations in flight, works directory by directory
without listing the tree first, never stops on single errors, and keeps a
journal so that a later run retries exactly what failed.

Status: work in progress. Local file systems, plain XRootD servers and EOS
instances work as source and target; EOS to EOS copies stream through the
client for now. [docs/design.md](docs/design.md) has the decisions behind the
behavior, [docs/architecture.md](docs/architecture.md) how it is built.

## Behavior in short

- A file is copied when it is missing on the target or differs in size or
  modification time. Otherwise only owner, group, mode and (for directories
  and symlinks) the modification time are fixed.
- Copies are atomic (temporary name plus rename, or EOS atomic uploads,
  except where EOS forces otherwise, see Known limitations), set the
  source's modification time, and verify a checksum end to end where the
  endpoint supports it. On local targets, a copy has mtime 0 until a sync of
  its file system (every 30 s, and at the end of a run) has made it durable,
  so that the next run, whatever it compares by, copies again any copy that a
  crash or a killed run left without its data. `--no-fsync` sets mtimes at
  once and syncs nothing. An EOS upload counts as done once the file it
  created, known by its id, is in place with the right size, mtime and
  checksum. A replacement that fails that check is given mtime 0 until the
  next attempt, which keeps the file's versions on EOS (it is removed instead
  where that fails, or where the source file's own mtime is 0). With a
  journal, the next run copies again an upload whose run ended before that.
- Symlinks are recreated as symlinks and never followed where the endpoint
  shows them (plain XRootD servers follow them, see Known limitations). Hard
  links are copied as separate files. Special files are skipped and reported.
- Extra entries on the target are deleted only with an explicit option
  (`--delete`), which behaves like rsync's: it deletes in directories that are
  empty on the source too, and takes an optional cap on the number of
  deletions (`--max-delete`). A run whose source is empty as a whole stops
  instead of emptying the target. `--keep-emptied` also keeps the entries of
  directories below that are empty on the source (as an unmounted automount
  looks). Nothing is deleted in directories where a listing had to skip
  entries, nor where the source, asked before deleting, has an entry its
  listing lacked. Those deletions, and the ones beyond the cap, are failures
  that `--retry-failed` takes up again. A source and target that overlap (one
  inside the other) are refused.
- Entries that a listing of the source has to skip (names that cannot be
  used as paths, lines of a server's listing that cannot be told apart)
  are not copied, which is a failure of their directory, as with rsync.
- Files and directories that disappear from the source during a run are
  counted as vanished, not as failures; the next run finds them gone.
- Errors are retried with backoff. What still fails is recorded in the journal
  and reported, and the exit status says so. When everything fails (an
  endpoint has gone away), the run stops after 1000 failures or vanished
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

eosmirror works between any two of local file systems, XRootD servers and EOS
instances. It copies a file when size or mtime differ, keeps symlinks, owners,
modes and mtimes, writes atomically with checksums verified end to end where
the endpoint computes them, retries and journals failures, and adapts its
concurrency to the target. EOS to EOS copies stream through the client;
third-party copy is a possible later addition.

## Differences from rsync

With `--delete`, eosmirror behaves like rsync wherever there is a choice,
with two deliberate exceptions: a run whose source is empty as a whole,
against a target that is not, stops instead of emptying the target (as an
unmounted file system or a broken server looks), and against a plain
XRootD target, which cannot show symlinks, `--delete` needs
`--target-without-symlinks` (see Known limitations).

## Usage

```
eosmirror sync [options] SOURCE TARGET
eosmirror selftest [--no-owner] [--target-without-symlinks] [--mgm URL] TARGET
eosmirror failures JOURNAL
eosmirror --version
```

`selftest` checks, in a temporary directory under the target, that
everything a run needs works there (creating files with verified checksums,
modes, owners, mtimes, symlinks, listings, removal) and says what the target
cannot do.

Endpoints are local paths or XRootD URLs. An XRootD URL that answers to EOS
commands is treated as EOS (symlinks, owners, nanosecond mtimes, atomic
uploads), and `eos://` names an EOS instance explicitly. Paths below `/eos`
are on the instance of `--mgm` or `EOS_MGM_URL`, as for the `eos` client. A
run logs both endpoints as resolved (server, real path, and where the MGM came
from). Authentication is the XRootD client's (`XrdSecPROTOCOL`, `XrdSecSSSKT`,
Kerberos tickets). eosmirror shows URLs without passwords and opaque
parameters, but XrdCl's own log (`XRD_LOGLEVEL`) shows passwords given in
URLs: give none there.

For example, a first run with a journal, then a rerun of what failed:

```
eosmirror sync --journal migration.db --progress 60 /data/project root://eos.example.org//eos/project
eosmirror sync --journal migration.db --retry-failed /data/project root://eos.example.org//eos/project
```

A journal serves one run at a time: a run whose journal another run holds
does not start.

Setting owners on EOS needs an identity that EOS treats as root (a sudoer is
not enough). Without one, the run refuses to start unless `--no-owner` is
given.

Plain XRootD servers take no mtimes and show no symlinks: a run with one as
target needs `--size-only` or `--checksum`, and skips and counts the source's
symlinks; one as source lists what its symlinks point to. As with rsync,
`--size-only` works on any target: it compares files by size alone, and sets
the mtimes of files that match where the target stores them.

Each file is copied in chunks of `--buffer-size` (8 MiB) with up to 4 reads
and 4 writes in flight (`--read-window`, `--write-window`). Writes go out in
order, each starting where the previous one ended, which is safe for
erasure-coded EOS files only under a condition (see Pipelined writes and
erasure coding). Every thread talks to XRootD and EOS servers over connections
of its own, since a server handles the requests of a connection one at a time
(`--shared-connection` shares one per server).

Files read from EOS or from an XRootD server that computes checksums are
verified against the stored checksum when the target computes none. Files read
from EOS take their metadata and checksum from the directory listing, so the
MGM sees one open per file, as with xrdcp. A file whose size changed since the
listing is stated before and after reading; one rewritten with the same size
shows up as a checksum that differs from the listed one, where the listing
carries one and copies are verified, and is stated then.

Up to 64 copies run at once (`--max-transfers`). A run starts with 4
(`--min-transfers`) and raises the number every 10 s while the throughput
improves, takes a raise back when the throughput drops after it, and lowers
the number when the target fails operations. `--transfers N` runs exactly N. A
copy holds up to both windows plus one buffer, so 64 copies hold up to 4.5 GiB
of buffers at the defaults (2.5 GiB between EOS and a local file system).

On a terminal, `sync` shows a live display: counters, the write rate, bars
for the transfer slots in use and the backlog, and one bar per file being
copied. A "listing-bound" count there and in the summary says how often
transfers had nothing to copy while every directory worker was busy: more
`--checkers` may help. `--progress SEC` prints plain progress lines instead
(for logs) and `-q` turns both off.

`eosmirror sync --help` lists the options: dry runs, deletion of extra entries
with a cap, parallelism (`--min-transfers`, `--max-transfers`, `--transfers`),
retries, resuming an interrupted run, sharding a tree over several hosts
(`--shard K/N`), symlink rewriting and staying on the file system of a local
source (`-x`, as with rsync: directories on other file systems are neither
walked nor deleted on the target).

The exit status is 0 when everything succeeded, 1 when some entries failed
after retries, 2 for usage errors, 3 when the run could not start (including
overlapping source and target and, with `--delete`, a source that is empty
while the target is not) or its journal failed, 4 when it stopped after too
many failures in a row (try again later with `--resume`) and 130 when
interrupted.

## Verifying a mirror

`--checksum` (`-c`) compares files of the same size by checksum instead
of mtime, as `rsync -c` does: by the checksums that both sides store where
they are of one type (EOS listings carry Adler-32; plain XRootD servers
may answer checksum queries), else by reading the files that store none
(Adler-32, or the type that the other side stores). Files that match get
their mtime fixed where it differs, and files that differ are copied. Where
the listings carry the checksums, this costs no requests beyond them.
Otherwise it asks plain XRootD servers that compute checksums, one request
per file, and reads the files that store none, which costs as much as
copying them.

With `--dry-run`, that is a verification that changes nothing on the target.
Every file that differs from the source (missing on the target, of another
size, or with another checksum) is reported as a failure, with both sizes or
checksums, so that the run exits with 1. The journal is kept up to date as by
a run (failures recorded, and those of files that now match cleared), so that
a later run with it copies those files again. The summary counts the files
compared by checksum and those that differ. Before the source of a migration
is retired, for example:

```
eosmirror sync --journal migration.db --dry-run --checksum /data/project root://eos.example.org//eos/project
eosmirror sync --journal migration.db --retry-failed /data/project root://eos.example.org//eos/project
```

## Pipelined writes and erasure coding

Each file is written with up to `--write-window` writes of `--buffer-size`
in flight (4 × 8 MiB by default), in offset order over one connection,
each starting where the previous one ended. xrdcp and `eos rclone` (which
copies through XrdCl's copy process) do the same, with 4 writes of 8 MiB.

For erasure-coded (RAIN) directories on EOS, this is safe only while the
storage nodes (FSTs) run with `xrootd.async off`, the default that EOS's
packaged FST configuration sets. XrdCl asks for asynchronous I/O on every
open, so an FST with `xrootd.async on` may execute the writes of a file out of
order, and EOS up to 5.5.2 then stores zero parity: the file reads correctly
until one of its stripes is lost. XrdCl never spreads writes over several
substreams, so `XRD_SUBSTREAMSPERCHANNEL` does not matter. A client cannot ask
an FST for the setting: when in doubt, use `--write-window 1`, which sends
each write once the previous one has completed and is slower over long
distances. Runs with an EOS target and a larger window say so at the start.

## Known limitations

- Plain XRootD servers cannot show symlinks: their listings follow them, and
  removing one removes the file it points to (XRootD's `XrdOssUnlink.cc`), so
  that deleting a directory through a symlink empties the directory it points
  to. `--delete` against a plain XRootD target is therefore refused unless
  `--target-without-symlinks` states that it has none, and so is replacing a
  file that the server cannot rename another over, as it cannot over a symlink:
  eosmirror would move it aside and remove it. Local and EOS targets show
  symlinks and remove them as such.
- EOS directories can force uploads to be non-atomic (`sys.forced.atomic=0`,
  or `user.forced.atomic=0` without the former). EOS then removes the file
  that eosmirror replaces at the start of the upload, and discards an upload
  that fails, so that a failed replacement leaves no file there until the next
  run copies it. Runs warn once per such directory where they replace files,
  and count those files. eosmirror does not see attributes that a directory
  takes from another through `sys.attr.link`.
- EOS keeps an atomic upload under a hidden name (`.sys.a#.`) until its close.
  With `--delete`, such files whose mtime and ctime are both more than an hour
  old are taken for leftovers of interrupted uploads and deleted, including
  another writer's upload that has been running for longer than an hour. EOS's
  own creation tracker (`eos space config default space.tracker=on`, off by
  default) removes abandoned atomic uploads after two days.
- The first Ctrl-C (or SIGTERM) stops the copies under way at their next
  chunk and discards what they wrote, waits for the requests in flight, makes
  the copies finished so far durable, and prints the summary. XrdCl's request
  timeout no longer applies once the header of a read's reply has arrived,
  so a storage node that sends the rest of the reply slowly keeps a transfer,
  and with it the run, waiting. A second Ctrl-C exits at once, without a
  summary. The next run removes the temporaries left behind (with
  `--delete`, once they are an hour old) and copies again what a sync has not
  made durable.
- Plain XRootD servers list names one per line, unescaped. A name with line
  breaks can add lines; names that then appear twice, and lines that cannot
  be names, are skipped and counted. XrdCl drops empty lines, though, so a
  name that starts or ends with a line break is listed without it (its copy
  fails as vanished), and one that is a line break alone is not listed at
  all, neither of them counted.
- Plain XRootD servers create files and directories with modes masked by
  their `ofs.crmode` (by default 0775), while eosmirror takes them to keep
  all permission bits: files writable by others (o+w) get their mode
  "fixed" by every run.

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
CERN's public EOS image; CI does so on demand. The XRootD tests also run
against an xrootd installed on the host, as in CI with Ubuntu's
`xrootd-server`:

```
EOSMIRROR_XROOTD_LOCAL=1 tests/integration/xrootd-server.sh start
EOSMIRROR_XROOTD_URL=root://localhost:1094//data ctest --test-dir build -R integration
EOSMIRROR_XROOTD_LOCAL=1 tests/integration/xrootd-server.sh stop
```

The EOS tests also run against a writable directory on another instance,
named by `EOSMIRROR_EOS_URL`, with the subdirectories `replica2`, `raid6`
and `nochecksum` as `tests/integration/eos/instance.sh` sets them up. Run as
a user whom EOS does not take for root, they leave out owners, and the
tests that need root on EOS (changing a file's layout) say that they are
skipped. The test of listings with find limits needs the limits for nobody
that `instance.sh` sets, and a caller that EOS lets become nobody.

Two longer tests (about a minute each) run only when asked for, as in
CI's on-demand job: `ctest --test-dir build -C long -L long`. One kills
syncs of a local tree at random points with SIGKILL and checks the copy
once a run completes. The other makes random changes to random trees,
syncs them with injected faults, checks that the journal records what the
faults left behind and that retries repair it, and compares the result in
content and metadata. Both print their seed; `EOSMIRROR_SEED=<seed>`
repeats the trees and changes of a failing one, though not where the kills
of the first test hit, so keep its output as well.

## Packaging

`packaging/eosmirror.spec` builds an RPM (`rpmbuild -ta eosmirror-VERSION.tar.gz`)
and `debian/` a Debian package (`dpkg-buildpackage -us -uc -b`).
`scripts/build-packages.sh [--output=DIR] [--image=IMAGE...] [PLATFORM...]`
builds both from the working tree: in the build containers, an RPM for EL9
and .debs for Ubuntu 22.04 and 24.04, and with `--image` in fresh containers
from stock Debian, Ubuntu, EL, Fedora or openSUSE images, against the
distribution's XRootD (EPEL's on EL). It installs each package built with
`--image` in a second fresh container and runs the self-test there:

```
scripts/build-packages.sh --image=docker.io/library/debian:13 \
  --image=docker.io/library/almalinux:9 --image=registry.fedoraproject.org/fedora:44
```

Debian 12, Ubuntu 22.04 and openSUSE Leap 16.0 ship no XRootD 5.6 or newer.

The XRootD client library comes from CERN's eos-xrootd (under
`/opt/eos/xrootd`, the XRootD that EOS is built against) or from the
distribution. The RPM uses eos-xrootd by default except on openSUSE
(`--with eos_xrootd` and `--without eos_xrootd` switch it), and the Debian
build uses it when it is installed.

## License

GPL-3.0-or-later, see [LICENSE](LICENSE). The bundled test framework
[doctest](https://github.com/doctest/doctest) is MIT licensed.
