# eosmirror design

eosmirror replicates a file tree one way, from a source to a target, between
POSIX file systems, XRootD servers and EOS instances. This note records the
decisions behind the code. Behavior that users see is in the README.

## What a run does

A run walks the source tree directory by directory and makes the target match:

- A file is copied when it is missing on the target or differs in size or
  mtime (compared at the coarser mtime resolution of the two endpoints).
  Otherwise only its owner, group and mode are fixed when they differ.
- A symlink is recreated when it is missing or points elsewhere, and its
  owner and mtime are fixed when they differ. Symlinks are never followed.
- A directory is created when missing; its owner, mode and mtime are set once
  all its entries have been processed, since changing entries changes mtimes.
- Special files (devices, sockets, fifos) are skipped and counted.
- Entries that exist only on the target are counted, and deleted only with
  `--delete`, up to `--max-delete` deletions per run.

Every copy writes to a temporary name (or uses EOS's atomic upload) and
renames into place, so an interrupted run never leaves a partial file under
the real name. Owner, mode and mtime are applied to the temporary file before
the rename, so a file never appears with wrong metadata either. The target
mtime is always the source mtime as observed *before* reading the file. If
the source changed while it was read (size or mtime differ afterwards), the
copy is reported as "changed" and retried; whatever was renamed into place
carries the old mtime, so a later run copies it again.

Checksums are computed while streaming and compared end to end where an
endpoint can provide one: EOS reports the checksum it computed while storing
the file, and stores one for reading. For a POSIX target, `--verify-readback`
reads the written file back.

## Structure

```
src/eosmirror/
  types.hh        Entry, Timespec, RelPath: what endpoints exchange
  error.hh        Error kinds, Result<T>, Status
  endpoint.hh     the Endpoint interface and file reader/writer interfaces
  posix_*.cc      POSIX endpoint
  engine.*        the walk, the queues and the worker pools
  copy.*          the per-file copy pipeline
  journal.*       SQLite journal
  report.*        counters and the final summary
  cli.*, main.cc  command line
```

### Endpoints

`Endpoint` is a small synchronous interface: list a directory with full
metadata in one call, stat, mkdir, symlink, set metadata, remove, rename,
open for reading, open for writing. Paths are relative to the endpoint's root.
Writers are atomic: `commit()` applies metadata, verifies the size and
checksum and renames into place; `abort()` removes the temporary file.

Each endpoint describes its capabilities: mtime resolution, whether owners
can be set, which checksum types it computes. The engine adapts to them
(comparison granularity, which checksum to compute, what preflight checks).

Endpoint operations are synchronous; concurrency comes from the worker pools
below. Remote endpoints may pipeline internally (for instance several writes
of one file in flight), which the interface allows because writes are
sequential and only `commit()` has to confirm them.

### Engine

Two pools of worker threads, like rclone's checkers and transfers:

- *Checkers* take directories from a stack, list source and target in one
  call each, compare the entries and act: metadata fixes and symlinks
  directly, subdirectories onto the stack, file copies into the transfer
  queue. The stack makes the walk depth first, so the number of pending
  directories stays bounded by depth times fan-out instead of growing with
  the width of the tree.
- *Transfers* take copy jobs from a bounded queue (`--max-backlog`) and run
  the copy pipeline. A full queue blocks the checkers, which keeps memory
  bounded and the transfer windows full.

Each directory has a completion counter: its own listing, each copy job and
each subdirectory. When it reaches zero, the directory is *finalized*: its
metadata is applied and the parent's counter is decremented. The run ends
when the root is finalized and the queues are drained.

Separate pools matter because a tree of tiny files is bound by metadata
latency and a tree of huge files by bandwidth. Later, the sizes of both pools
adapt to measured throughput.

### Retries and the journal

Operations are retried with exponential backoff while the error is
transient (I/O errors, timeouts, the source changing). What still fails is
recorded in the journal (`--journal FILE`, SQLite) with the path, the kind of
entry and the last error, and the run goes on. Finalized directories are
recorded too. Modes:

- A plain run with a journal walks everything. Paths that succeed drop out of
  the failure table, paths that fail replace their row.
- `--resume` skips directories finalized by the interrupted run with the
  same source and target.
- `--retry-failed` processes only the recorded failures: single files as
  copy jobs, directories as subtree walks.

### Sharding

`--shard K/N` runs the same walk on N hosts. Every shard creates the
directory skeleton (mkdir is idempotent), but files, symlinks, metadata and
deletions of a directory are handled only by the shard that owns it,
chosen by a hash of the directory path. Directory listings are repeated per
shard, which is cheap compared to the copies.

### Reporting

Counters for everything that happened (directories, files copied with
bytes, unchanged, metadata fixed, symlinks, skipped special files, deleted,
failed by kind, retries) go to a summary at the end and optionally to a
periodic progress line. Exit status: 0 when everything succeeded, 1 when
something failed after retries, 2 for usage errors, 3 when the run could not
start (unreadable source, unusable journal, failed preflight), 130 when
interrupted.

## EOS specifics (milestones 2 and 3)

- XRootD has no symlink, chown or utimes operations and its stat lacks
  owners, modes and link targets. The EOS endpoint uses MGM commands sent as
  XRootD queries, the way the `eos` client does, and lists directories with
  `find --format`.
- Files are written through XRootD with one writer per file and sequential
  offsets. EOS computes the checksum while storing the file and returns it
  at close. Erasure-coded layouts require this write order (EOS up to 5.5.2
  stores zero parity for files written out of order), so ranges of a file
  are never written in parallel and a target that is an EOS FUSE mount is
  warned about.
- mtimes are set at creation (`eos.mtime`) and fixed with `utimes` for
  existing entries. Owners need a privileged identity (root via sss, or a
  sudoer); preflight checks for it.
- The directory's layout and checksum settings decide how a file is stored;
  the tool passes no layout hints.
- EOS to EOS copies use XRootD third-party copy where available, with
  streaming through the client as the fallback.

## Not in scope (for now)

Extended attributes, ACLs, hard links (copied as separate files), delta or
partial updates, checksum-based change detection, id mapping between sites.
