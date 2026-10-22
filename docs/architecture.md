# eosmirror architecture

How eosmirror is built: the parts, what runs where, the states a copy goes
through, the invariants every change must keep, and how the tests protect
them. What users see is in the [README](../README.md); the decisions behind
the behavior, with the EOS and XRootD specifics, are in [design.md](design.md).

A run reconciles one tree with another, directory by directory, without a
snapshot or a transaction over the tree: the source may change while it runs,
and the engine notices what it can (a file read that changed meanwhile fails
and is retried, one that changed since its listing is copied again by the
next run). Trees are replicated as far as the target can hold them, which
its capabilities say.

## Parts

```
src/eosmirror/
  types, error, checksum, buffer     the vocabulary: Entry, RelPath, Timespec, ErrorKind,
                                     Error, Result<T>/Status, Checksum/Hasher, Buffer/Chunk/BufferPool
  endpoint.hh                        the Endpoint, FileReader and FileWriter interfaces,
                                     Capabilities, Listing, CommitSpec, Location
  posix_endpoint                     local file systems, deferred mtimes (DeferredMtimes)
  xrootd_endpoint                    plain XRootD servers through XrdCl; XrdReader/XrdFileWriter
  eos_endpoint                       EOS on top of XrdEndpoint: MGM commands, find listings, uploads
  atomic_upload                      AtomicUploadWriter: a commit that the endpoint confirms afterwards
  pipeline                           ReadAhead and WriteWindow: reads and writes kept in flight
  endpoints                          the factory for command line specs, the overlap check
  copy                               copy_file(): one file from a source to a target; read_checksum()
  engine                             the walk, the decisions, the worker pools, the journal bookkeeping
  queue, retry, cancellation         WorkQueue, with_retries(), Cancellation
  journal                            the SQLite journal
  report, progress_display, log      counters, failures, the summary, the live display, logging
  options, cli, main                 SyncOptions and RetryPolicy, the command line, the commands
  selftest                           `eosmirror selftest`: a target exercised through the interface
```

Dependencies point one way, with no cycles:

```
main ──► cli ──► endpoints ──► posix_endpoint ──────────────┐
 │        │         └──(with XRootD)──► eos_endpoint ──► xrootd_endpoint ──► pipeline
 │        │                                 └──► atomic_upload ─┐                │
 │        └──► journal ──► report ──┐                           ▼                ▼
 ├──► engine ──► copy ──────────────┴──► endpoint ──► types, error, checksum, buffer
 │      └──► queue, retry, cancellation, journal, report, log
 ├──► progress_display ──► report, log
 └──► selftest ──► endpoint
```

The engine and `copy_file()` know no endpoint type. What they know about a
target or source comes through `Capabilities` (mtime resolution, owners,
settable fields, symlinks, the checksum computed, whether files are in place
before their commit is confirmed), through `Listing` (entries left out,
whether files are replaced at their open) and through the virtual hooks
`is_temporary()`, `other_file_system()`, `FileReader::state()` and
`unchanged()`. Each has a meaning of its own; that EOS or XRootD motivated it
is in design.md. `AtomicUploadWriter` is generic in the same sense: the test
endpoint uses it as EOS does.

## A run

`main.cc` parses the command line (`cli.cc`), makes the two endpoints
(`make_endpoint()`, which probes XRootD URLs for an EOS MGM), logs both as
resolved, refuses overlapping trees (`check_disjoint()`) and a journal inside
either tree, creates the target's missing parents, opens the journal
(`open_journal()`: owner check, lock, the options of an interrupted run, the
run row), installs the signal handlers and runs the `Engine`. Afterwards it
ends the journal's run, prints the summary and maps the outcome to the exit
status (`ExitCode` in main.cc; the table is in design.md). An engine runs
once and borrows the endpoints, the report, the journal and the cancellation,
which outlive it.

`Engine::run()` (engine.cc, `Impl::run_tree()`) stats the source root, makes
the root `DirNode` (creating the target root if needed), loads the journal's
failures into `prior_failures`, settles what the endpoints can do
(`adapt_to_capabilities()`, which probes the target's mtime resolution), and
starts the threads (`run_nodes()`):

- `checkers` (default 8) pop directories from `dirs`, a LIFO queue, so that
  the walk is depth first and the pending directories stay bounded by depth
  times fan-out. `process_dir()` lists the source and the target, compares
  entry by entry (`handle_dir()`, `handle_file()`, `handle_symlink()`),
  pushes subdirectories onto `dirs` and copy jobs onto `copies`, then handles
  the target's extras (`handle_extras()`) and the directory's own completion.
- `transfers` (`--max-transfers`, default 64) pop `CopyJob`s from `copies`, a
  FIFO queue bounded by `--max-backlog` (a full queue blocks the checkers),
  take a slot of the `Gate`, which limits how many run at once, and run
  `run_copy()`. The slot is released before the directory's completion is
  counted.
- the controller (only with a range of transfers) adjusts the gate's limit
  every `adapt_interval` from the throughput and the target's errors
  (`TransferController`, a pure class with its own tests).
- the syncer (not in dry runs) calls `sync_target()` every `sync_interval`:
  the target's `flush()` makes the copies so far durable (local targets), and
  only then are the directories finalized before the sync recorded in the
  journal as done.

The run ends when the root is finalized (`roots_pending == 0`) or the run is
cancelled. Then the queues are closed and drained, the threads joined, a last
`sync_target()` made, and `end_error` (the first fatal error) or the
cancellation becomes the result. `Engine::run(entries)` is `--retry-failed`:
the journal's rows grouped by parent directory, each group a `DirNode` with
`only` set to its names, directories among them walked whole, the top
directory as a whole tree.

### Directory completion

A `DirNode` lives from its listing until everything in it is done. Its
`pending` counter starts at 1 for its own processing and takes one more for
every subdirectory pushed and every copy queued, before the work is
published; `entry_done()` discharges one, and the thread that brings the
counter to zero, a checker or a transfer, runs `finalize()`: the directory's
metadata is applied (mtime last, since changing entries changes it), its
earlier failure row is cleared if it completed, it is queued for the journal
(after the next sync) and its parent's counter is decremented. Cancellation
abandons the work still queued: the queues are drained and the run ends by
joining the threads, not by completing the root.

The fields of a node that only the checker processing it writes (`unlisted`,
`deletions_held`, `replaces_at_open`, `only`, `created`, `target`) are plain;
those that transfers or children may set (`touched`, `failed`, `incomplete`,
the warned-once flags, `made_writable`) are atomics.

### A copy

`handle_file()` decides: a file is copied when it is missing on the target,
differs in size, or in mtime at the coarser resolution of the two endpoints
(`same_mtime()`), unless `--size-only` or `--checksum` say otherwise; a copy
in place that is provisional (mtime 0 while the source's is not,
`provisional()`) or in doubt (a journal row of an in-doubt kind,
`copy_in_doubt()`) is copied again whatever the comparison says, after
getting mtime 0 (`invalidate_in_doubt()`) so that it can never pass for good
if the new copy does not arrive. With `--checksum`, stored checksums of one
type decide at once; otherwise the transfer reads what is not stored
(`differs_by_checksum()`).

`run_copy()` runs `copy_file()` under `with_retries()`: open the source,
take its state (`FileReader::state()`: a stat, or the listed entry where the
reader can tell that the file still has its listed size), read the first
chunk before the target is touched, open the target, stream chunks through
the transfer's `BufferPool` while hashing, check the source `unchanged()`,
compare the source's stored checksum where the target computes none or the
listing carries one, record the pending journal row where the target
confirms after commit (once per file, though `before_commit` runs in every
attempt), and `commit()`. Every failure path goes through the `aborted`
lambda, which asks the writer to discard what it wrote and, where that may
have failed, marks the error in doubt. The result is counted, clears or
writes the journal row, and the directory's completion is counted.

Within a file, remote endpoints pipeline: `ReadAhead` keeps up to
`--read-window` chunk reads in flight and hands them over in order,
`WriteWindow` keeps up to `--write-window` writes in flight, each starting
where the previous one ended, and once one has failed nothing more is sent
and the rest is waited for before the file is closed. Both are transport
independent (pipeline.cc) and tested with synthetic submit functions; the
XrdCl specifics are in `XrdReader` and `XrdFileWriter`.

### Durability and confirmation

Nothing appears under a final name before it is complete and verified, and
nothing passes for a good copy before it is confirmed. How, differs by
target:

- Local (`PosixWriter::finish()`): data to a temporary name, writeback
  started, optional readback, owner and mode applied, the rename, then the
  file has mtime 0 until a `syncfs` of its file system (`DeferredMtimes`,
  every 30 s and at the end) is followed by the mtime, set through a
  descriptor whose device and inode are the copy's. A source file with mtime
  0 is fsynced and gets its mtime before the rename, and its directory is
  fsynced after it. `--no-fsync` sets mtimes before the rename and syncs
  nothing.
- Plain XRootD (`XrdWriter`): a temporary name, the stored checksum (or the
  size) checked after the close, the mode set, the rename. A server that
  refuses to replace a file gets the old one moved aside first
  (`move_replacing()`), so there the old file is briefly under a temporary
  name.
- EOS (`EosUploadWriter` inside an `AtomicUploadWriter`): EOS's atomic upload
  under a hidden name, renamed into place by the storage node at close, so
  the file is in place before it is confirmed. The upload is known by the id
  of the file it created (`mgm.id` in the open's redirect). After the close,
  one query by that id (`EosEndpoint::file_by_id()`, getfmd) tells whether
  that file is in place under the name with the size, mtime and checksum of
  the copy; then owner and mode are set. An upload that is not confirmed, or
  whose close or abort failed, is taken back (`take_back()`): mtime 0 where
  it replaced a file (removing would purge EOS's versions; removed all the
  same where that fails or the source's mtime is 0), removed where it
  replaced nothing or is still hidden, left alone where it cannot be told
  from another file, which leaves the copy in doubt. A directory that
  forces non-atomic uploads removes the old file at the open instead, and a
  failed replacement leaves nothing.

Three things called "complete" mean different things: a writer's `commit()`
succeeded (the endpoint has the copy, locally still with mtime 0 until the
sync); a directory is finalized (its own processing and its entries are
done, failures among them recorded); a run completed (the engine finished,
entries may have failed, the exit status says so).

## States

### A target file

For a path that is a file on the source, the target holds one of:

| State | How it got there | What the engine does with it |
|---|---|---|
| absent | never copied, deleted, vanished | copied |
| temporary | `.<name>.eosmirror-<hex>`, or EOS's `.sys.a#.` upload | never an entry; removed with `--delete` once stale (1 h by mtime and ctime) |
| provisional (mtime 0) | a local copy before its sync; an EOS replacement taken back; `invalidate_in_doubt()` | copied again, whatever the comparison |
| in doubt | a journal row of kind IO, Timeout or Checksum: the pending row before an EOS close, a failed confirmation that could not be taken back, a failed invalidation | copied again by any run with the journal that visits it, after mtime 0 |
| good | size and mtime (or checksum) match and it was confirmed | unchanged; owner, group, mode fixed |
| differing | `--checksum` found another checksum | dry run: a Checksum failure row; run: mtime 0, then copied |
| other type | a directory or symlink under the name | replaced only with `--delete`, after a stat of the source confirms the type |
| replaced at open | an EOS directory that forces non-atomic uploads removed the file at the open | copied; a failed replacement leaves nothing, warned once per directory and counted |

A copy's mtime is always the source's mtime as observed before the data was
read; a source that changed by the end fails the copy (`Changed`), and
whatever was renamed carries the old mtime, so the next run copies it again.

### A journal row

The journal (`journal.cc`, SQLite in WAL mode with `synchronous=NORMAL`, so
rows survive the process but not necessarily a power loss) holds
`failures(path, type, kind, message, run_id, updated)`, `done_dirs(path,
run_id)`, `runs(started, finished, completed)` and `meta` (source, target,
shard, the options of the last run). Its version (SQLite's `user_version`,
`kVersion` in journal.cc) rises with any change to the tables or to what
their rows mean, and eosmirror refuses journals of a newer version. The kind
of a failure row is the `ErrorKind` as a number, and the type the
`EntryType`, which is why new values of both go last. Three groups of kinds
matter:

- in doubt (`may_be_in_doubt()`): IO, Timeout, Checksum. The copy in place
  may be the failed one with the source's size and mtime. Every run with the
  journal that visits the file copies it again (`copy_in_doubt()`), and its
  source vanishing does not clear the row while the file is in place.
  `Error::in_doubt` itself is not stored: the paths that leave a copy in
  doubt record a row of one of these kinds, IO unless the error has one.
- Withheld: a deletion held back (incomplete listings, the source still has
  the entry, the `--max-delete` cap). `--retry-failed` with `--delete`
  takes it up; it counts as no failure in a row.
- everything else: a failure to retry by `--retry-failed`.

Rows are written by `fail()` (replacing an earlier row for the path), by
`differs()` in a verifying dry run, and before an EOS close (the pending row).
They are cleared by `succeeded()` for a path that had a row (copied, metadata
fixed, deleted, a directory completed), by `settled()` once an upload is
confirmed, by `clear_vanished()` for names gone from both sides of a
completely listed directory, by `vanished()` unless the copy is in doubt, and
by `retry_without_source()` where the target lacks the entry too. `Cancelled`
is never recorded. A run that cannot write the journal stops (exit 3).

`done_dirs` rows are written after the sync that followed a directory's
finalization and dropped when a run begins, unless it resumes an interrupted
one (`runs.completed` null or 0). A done directory can hold recorded
failures: `--resume` selects no rows, it skips the directories recorded as
done and handles the rest like any run, so the failures there are handled
on the way and those under done directories wait for `--retry-failed`. Dry
runs open the journal read-only and change nothing, except a verifying one
(`--dry-run --checksum`), which keeps the failures up to date and records no
directory.

## Threads and shared state

| Thread | Started by | Does |
|---|---|---|
| main | | setup, `Engine::run()`, summary |
| progress | main, with `--progress` | prints a progress line per interval |
| display | main, on a terminal | redraws the live display, steps aside for log lines (`log::set_display_hooks`) |
| checkers | engine | directory walk |
| transfers | engine | copies; the `Gate` limits how many are active |
| controller | engine, with adaptive transfers | the transfer limit |
| syncer | engine, not in dry runs | `sync_target()` every interval, while transfers go on |
| XrdCl's | the client library | completion handlers (`Callback`) into `ReadAhead`, `WriteWindow` and listings |
| mtime workers | `DeferredMtimes::flush()` | up to 8, apply mtimes after a sync |
| signal handler | | `Cancellation::request()`; the second signal exits |

Shared state and its protection: `WorkQueue` and `Gate` (own mutexes);
`Stats` (atomics); `DirNode` (see above); `prior_failures` (filled before the
workers start, read-only afterwards, also while the journal changes);
`finalized` and `sync_failed` (`finalized_mutex`, atomic); `end_error`
(`end_mutex`); `Report`'s failures and the log (own mutexes); the journal
(its mutex, one statement at a time); `XrdEndpoint::fs_` (a `FileSystem` per
thread under `fs_mutex_`, so that each thread's requests go over connections
of its own); `ReadAhead` and `WriteWindow` (own mutexes, completions from
XrdCl's threads); `DeferredMtimes` (a mutex for the pending copies, one flush
at a time). `Cancellation` is polled, since a signal handler sets it.

Two rules follow from this. The display renders under the log mutex and
takes a transfer slot's mutex while doing so, so a worker holds a slot's
mutex only to set the path and size, never while logging. XrdCl's completion
threads do only the pipeline's bookkeeping: a journal write, a log line
that waits for the display, or a blocking throttle there would hold up the
callbacks that release buffers.

Memory: the copies waiting are bounded by `--max-backlog`, the buffers by the
windows (a transfer keeps the buffers it allocated for its next copies). The
rest grows with the data, not with the tree: each directory being processed
holds both listings whole, the directory stack holds the frontier of the
walk, `prior_failures` holds every row of the journal, and the copies
finalized since the last sync wait in `finalized` and in `DeferredMtimes`.
The directory queue has no capacity on purpose: checkers both take from it
and push to it, so a full one would deadlock them.

## Errors

`Result<T>`/`Status` carry an `Error{kind, message, errnum, in_doubt}`; no
exceptions are thrown for control flow (the only ones caught are thread
creation failures). `ErrorKind` decides three things: whether an operation is
retried (`is_transient()`: IO, Timeout, Changed and Checksum;
`XrdEndpoint::xrd_error()` maps busy servers and connection errors to IO),
what a journal row means (above), and how a run ends (`Cancelled` ends it
without a row; `main` tells the engine's own stop after failures in a row
from a signal by the caller's cancellation). `upload_error()` turns the
failure of a write or close into a transient IO error unless space or
permission is missing, since the target then holds no file, or one that the
writer takes back. `Error::in_doubt` is set where a failed copy may have left
its data in place with the source's size and mtime; `run_copy()` turns it
into an in-doubt row however the later attempts end, also when cancelled.
The transient errors of the target are also what the controller lowers the
transfer limit for (`on_target_error`).

## Invariants

Every change keeps these; the tests named protect them.

1. Nothing is visible under a final name before it is complete and verified,
   except where the target replaces at the open (warned and counted).
   `test_posix_endpoint` "commits atomically", `test_engine` "nothing is
   created on the target before the first chunk was read", "a file that
   changes during the copy is not committed".
2. A copy passes for good only once confirmed: until then it has mtime 0 or
   an in-doubt journal row, and every later run that visits it copies it
   again whatever the comparison mode. "a copy with mtime 0 is made again,
   whatever the comparison says", "files whose last copy failed are copied
   again", "a copy stays in doubt however its later attempts end",
   `tests/long/crash.sh`.
3. The target's mtime is the source's as observed before reading; a source
   that changed during the read is not committed.
4. Names from listings are untrusted: validated before they are joined into
   paths (`valid_entry_name()`), printed through `printable()`, and listing
   parsers refuse what they cannot tell apart. "listed names that cannot be
   joined into paths are skipped", `test_xrootd_eos` "find listing lines
   that are refused".
5. Nothing is deleted without `--delete`; never everything (the empty-source
   stop); nothing in a directory whose listings may be incomplete; no extra
   before a stat of the source confirmed its absence; every deletion held
   back is a journaled failure. "an empty source does not empty the target",
   "nothing is deleted where a listing skipped entries", "the source confirms
   what it lacks before anything is deleted".
6. The journal is the only state across runs: it belongs to one source,
   target and shard, serves one run at a time, and a run that cannot write
   it stops. `test_journal`, "a run stops when its journal cannot be written".
7. Entries vanished from the source are no failures but count towards the
   stop after failures in a row. "entries that vanish from the source ... are
   no failures", "a run stops after too many failures in a row".
8. A directory's metadata is applied once its entries are done, and a
   directory is recorded as finalized only after the sync that followed.
   "directory mtimes are applied after their entries", "directories are
   recorded as finalized once the target has synced their copies".
9. Shards all create the directory skeleton; only the owner of a directory
   changes its entries. "shards partition the files and together cover
   everything", the three shards of `tests/cli/smoke.sh`.
10. The source is never written to. "a source is never written to".
11. Writes to a file go in offset order over one connection, each where the
    previous one ended (erasure-coded EOS files), with XrdCl's write recovery
    off. `test_pipeline` "write windows start writes in order", `test_eos`
    "pipelined writes give erasure-coded files their parity".
12. The copies made so far stay when a run is cancelled; the first signal
    waits for the requests in flight and syncs, the second exits at once.
    "cancellation stops the run and leaves nothing partial".

## The endpoint contract

An `Endpoint` implementation guarantees:

- `list()` returns every entry with its full metadata (type, size, mtime,
  ctime where known, owner, mode, link target, directory identity, stored
  checksum where the listing carries one), counts in `skipped` what it had
  to leave out, and says whether files in the directory are replaced at
  their open. Comparing entries takes no stat; deleting does (the source is
  asked about every extra).
- `stat()` does not follow a final symlink where the endpoint shows symlinks
  (plain XRootD servers follow them and say so in `follows_symlinks`; the
  root itself may be a symlink to the tree on POSIX).
- `open_write()` makes nothing visible under the final name until
  `commit()`, which applies the metadata, verifies the size and the checksum
  the endpoint stores, and puts the file in place or fails with it not in
  place, except where `confirms_after_commit` says that the file is in
  place first; `abort()` discards what was written and reports, as an error
  marked `in_doubt`, where the data may stay in place all the same.
- `is_temporary()` recognizes the endpoint's own temporaries, which listings
  of a target never report as foreign entries, and which are stale after an
  hour by mtime and ctime.
- `capabilities()` is honest: the mtime resolution, whether owners exist and
  can be set, whether modes can be set and which of their bits are stored,
  whether mtimes can be set, whether symlinks exist and have owners, whether
  listings follow symlinks the endpoint cannot show, which checksum it
  computes, and whether a committed file is in place before `commit()`
  confirmed it.
- Errors are `ErrorKind`s with the errno where known: `NotFound` for an
  absent entry, `Exists` for a `mkdir` that lost a race, `Changed` for a
  source that is not what was listed, transient kinds only where a retry can
  help.
- All methods may be called from many threads at once, `flush()` while
  transfers write. A reader or writer is used by one thread, while the
  endpoint may complete its requests from others, and it is destroyed only
  once those requests are done.

The test endpoint (`tests/unit/fake_endpoint.hh`) models the behaviors the
engine has to cope with, one switch each; a behavior found in a real
endpoint gets a switch and an engine test. Faults fail one operation on one
path a given number of times; the `hook` runs before the fault check, from
whichever thread makes the call, and may change the tree. Uploads go through
the production `AtomicUploadWriter`, so those tests cover the engine's use
of it, not an independent model of EOS.

| Switch | Real behavior |
|---|---|
| `atomic_uploads`, `hidden_uploads`, `committing_aborts`, the `lost` and `close` faults | EOS uploads in place before confirmation, a close that timed out, a delete request that failed, a close reported done after a lost connection |
| `readers_take_listing` | EOS readers take the listed entry as the file's state |
| `lists_checksums` | EOS listings carry adler32 checksums |
| `replacing_at_open` | EOS directories that force non-atomic uploads |
| `utimes_needs_write`, `entries_need_write` | EOS and POSIX as a user other than root |
| `unlistable`, `unlisted` | lines an EOS listing cannot parse; a plain XRootD listing that ends early |
| directory id `other-fs` | a mount point below a local source |

## Tests

- Unit (`tests/unit`, doctest, `ctest`): the engine on `FakeEndpoint` with
  fault injection (test_engine.cc), the local endpoint on a temporary
  directory, the parsers and URL handling of the XRootD and EOS endpoints
  without a server, the pipeline with synthetic submits, the journal, the
  CLI, the controller. `unit_unprivileged` runs the tests of what a user
  other than root cannot do, as `nobody` where needed (skipped with exit
  status 77 where it cannot). `cli_smoke` runs the binary on local
  directories, three shards among them.
- Integration (`tests/integration`, `scripts/integration-test.sh`): against
  a plain xrootd server and a single-host EOS instance in containers from
  CERN's public image (`tests/integration/eos/instance.sh`): the endpoints'
  operations, uploads and downloads of many chunks, erasure-coded parity
  (read back with stripes dropped, since a normal read cannot tell), the
  EOS-specific failure paths, the self-test, a sudoer's identity.
- Long (`ctest -C long -L long`): the randomized differential test (random
  trees, random changes, injected faults; the journal must record what was
  left behind before a clean run repairs it, and the target must then equal
  the source) and the crash test (SIGKILL at random points, then runs until
  one completes; the copy must equal the source). A seed repeats the trees
  and the changes, not where the kills hit. SIGKILL leaves the page cache
  and SQLite's files intact: this tests the process dying, not a power loss.
- Sanitizers: `scripts/check.sh PLATFORM address|thread|undefined`;
  undefined behavior fails the tests; `tests/lsan.supp` and `tests/tsan.supp`
  suppress what is in XRootD's, OpenSSL's and glibc's own code.

CI (`.github/workflows/ci.yml`): on every push the unit tests on Ubuntu 22.04
(plain, address, thread; without XRootD, whose Ubuntu package is too old),
Ubuntu 24.04 (plain, thread; with the xrootd integration tests against the
distribution's server) and EL9 (plain, address), and the packages for EL9 and
Ubuntu, plus packages from stock Debian, Ubuntu, EL, Fedora and openSUSE
images, each installed in a fresh container with the self-test run; nightly,
the long tests and the EOS integration tests in the build containers.

What a change needs: a sanitizer build (address and thread) for anything in
the engine, the pipeline, the endpoints or the journal; the integration tests
for the endpoints and `copy.cc`; the long tests for the engine, the journal
and the local endpoint's durability.

## Extending

- A new option: `parse_command_line()` and `usage()` (cli.cc), `SyncOptions`
  or `EndpointSettings`, and `result_options()` if it decides what a run
  makes of the target (a resumed run must share it); the README's option
  list; design.md where it is a decision.
- A new `ErrorKind` or `EntryType`: last in the enum (journals store the
  number), `to_string()`, and for a kind `is_transient()` and whether it
  is an in-doubt kind.
- A new counter: `Stats`, the summary and the progress line in report.cc.
- A new endpoint type: `Endpoint`, `FileReader` and `FileWriter` with the
  contract above, a scheme in `make_endpoint()`, a `Location` the overlap
  check can compare, a `FakeEndpoint` switch for every behavior of it that
  the engine must handle, integration tests, and `eosmirror selftest` passing
  against it. The engine and `copy.cc` should need no change; a change there
  means a capability is missing. An endpoint that publishes a file before
  its close, or stores no mtimes a copy can set, needs its recovery settled
  first, not a flag that makes it look like EOS.
- Scheduling: a bandwidth cap belongs before a chunk is submitted, a rate
  limit at the worker's operation; a pause is a state of its own, since
  cancellation drains the queues; a second target is a second engine.
- Planned extensions and where they land: third-party copy (an operation on
  `Endpoint` for a copy between two remote endpoints, beside `copy_file()`,
  with the confirmation by id kept); extended attributes and ACLs (`Entry`
  and `MetaFields`; EOS listings already carry `attr.*` fields); hard links
  (`Entry` needs a link count and a file identity, and links cross the
  directory-local walk and shard ownership).
