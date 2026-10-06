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

Names from listings are checked before they are joined into paths: empty
names, `.`, `..` and names containing `/`, NUL or a line break are skipped
with a warning and counted, on both sides and when deleting trees. Log lines,
the progress display and the summary replace control characters and invalid
UTF-8 in names with `?`.

Each endpoint describes its capabilities: mtime resolution (POSIX probes
it on the target), whether owners, modes (and which mode bits) and mtimes
can be set, whether symlinks exist and have owners, which checksum it
computes. The engine adapts: comparison granularity, size-only comparison
without mtimes, skipped symlinks, masked mode comparison, which checksum to
compute, what the preflight checks. Without the privilege to set owners,
the engine makes read-only target directories writable while it changes
their entries and restores their mode afterwards.

`eosmirror selftest TARGET` exercises all of this in a temporary directory
under a target before a long run and reports what the target cannot do.

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

- A plain run with a journal walks everything. Paths that succeed (copied,
  metadata fixed, deleted, or gone from both sides) drop out of the failure
  table, paths that fail replace their row.
- `--resume` skips directories finalized by the previous run if that run
  was interrupted; after a completed run there is nothing to resume and
  everything is walked. Recorded failures are not retried by resuming.
- `--retry-failed` processes only the recorded failures: each within its
  parent directory, failed directories as subtree walks (rows below them are
  covered by that walk).
- A journal belongs to one source, target and shard, with local paths in
  canonical form; dry runs read it but never change it.

### Sharding

`--shard K/N` runs the same walk on N hosts. Every shard creates the
directory skeleton (mkdir is idempotent), but files, symlinks, metadata and
deletions of a directory are handled only by the shard that owns it,
chosen by a hash of the directory path. Directory listings are repeated per
shard, which is cheap compared to the copies.

### Reporting

Counters for everything that happened (directories, files copied with
bytes, unchanged, metadata fixed, symlinks, skipped special files, deleted,
skipped invalid names, failed by kind, retries) go to a summary at the end
and optionally to a periodic progress line. Exit status: 0 when everything succeeded, 1 when
something failed after retries, 2 for usage errors, 3 when the run could not
start (unreadable source, unusable journal, failed preflight), 130 when
interrupted.

## XRootD endpoint

Plain XRootD servers list directories with per-entry stat in one call, store
no symlinks, cannot set owners or mtimes, and compute a checksum only when
configured (`query config chksum` tells which). The endpoint reports these
as capabilities; the engine then compares files by size only, skips symlinks
(counted), and applies no mtimes. Writes go to a temporary name with up to
four writes in flight per file, the stored checksum is verified by a checksum
query, and the file is renamed into place.

XRootD has no escaping for paths: the server takes everything after the
first `?` as opaque parameters, in opens as in all other requests. Paths
containing `?` are therefore refused with an error instead of being sent.

## EOS specifics (milestone 3)

XRootD has no symlink, chown or utimes operations and its stat lacks owners,
modes and link targets. The EOS endpoint uses the MGM's commands, sent the
way the `eos` client sends them (EOS 5.5 source, `console/` and `mgm/proc/`):

- Paths are sent percent-encoded behind `/#curl#` (`/#curl#/eos/a%20b`),
  with `eos.encodepath=1` among the opaque parameters: in opens, in plain
  XRootD requests (mkdir, rm, rmdir, checksum queries, which take
  `<path>?eos.encodepath=1`), in OpaqueFile queries and as `mgm.path` of
  commands. The MGM decodes only paths with this prefix.
- Text commands are opened as the file `root://mgm//proc/user/?mgm.cmd=...`
  and read; the reply is `mgm.proc.stdout=...&mgm.proc.stderr=...&mgm.proc.retc=N`
  with N an errno. Protobuf commands are sent the same way as
  `mgm.cmd.proto=<base64 of a RequestProto>`. The output of protobuf
  commands is not escaped and can contain the markers, so they are searched
  from the end of the reply.
- Listing: `find` (RequestProto field 5, FindProto) with `Files`,
  `Directories`, `Maxdepth=1` (a oneof, so it must be sent), `Path` and
  `Format="type,size,uid,gid,mode,flags,mtime,link"`, plus
  `SkipVersionDirs`. Each output line is `path="<abs>" type=... size=N
  uid=U gid=G mode=<octal, directories> flags=<octal, files>
  mtime=<sec>.<nsec> target="<link target>"`, the fields in the order of the
  format and the target only for symlinks.
  The start directory itself is listed too and skipped; a listing without it
  is an error (find prints paths below the real path, which differs when the
  path leads through a symlink). Paths and targets are printed raw: lines
  with more than one `" type=` (which a name or target can contain), names
  with control characters, entries outside the directory and names listed
  twice are skipped with a warning.
- Single stat: `mgm.pcmd=stat` as an OpaqueFile query on the path; the reply
  `stat: dev ino mode nlink uid gid rdev size blksize blocks atime mtime
  ctime atime_ns mtime_ns ctime_ns` has nanoseconds.
- Symlink: `mgm.cmd=file&mgm.subcmd=symlink&mgm.path=<encoded>&eos.encodepath=1
  &mgm.file.source=<path>&mgm.file.target=<target>`. The MGM takes source
  and target verbatim (EOS 5.5.2 `mgm/proc/user/File.cc`), so they are sent
  unencoded.
- mtime: `eos.mtime=<sec>.<nsec>` on the open URL for new files; otherwise
  the OpaqueFile query `<encoded path>?eos.encodepath=1&mgm.pcmd=utimes
  &tv1_sec=0&tv1_nsec=0&tv2_sec=S&tv2_nsec=<9 digits>`, which works for
  directories and symlinks as well.
- Owner and mode: `mgm.cmd=chown&mgm.path=...&mgm.chown.owner=uid:gid` and
  `mgm.cmd=chmod&mgm.path=...&mgm.chmod.mode=<octal>`; chown needs a root or
  sudoer identity, which the preflight checks.
- Writes use EOS's atomic upload (`eos.atomic=1` on the open URL, with
  `eos.mtime`), so EOS itself renames the file into place at close and a
  close before the end discards the upload. The checksum is read back with
  a checksum query and compared when the directory's `sys.forced.checksum`
  attribute (cached per directory) names the type that was computed. Owner
  and mode are applied after the close, since an upload runs under the
  client's identity: the file is briefly visible with the uploader's owner.
- EOS cannot change the owner of a symlink (chown follows the link) and
  cannot replace one in place, so symlink owners are not synchronized and a
  changed symlink is removed and recreated. Since the symlink command takes
  its source path and target unencoded, a symlink whose path or target
  contains `&`, `=`, `?`, `#`, `%` or a control character cannot be created
  and is reported as a failure.
- Whether the identity may set owners comes from `whoami` (root or
  `sudo*`).
- EOS's own hidden entries (atomic temporaries `.sys.a#.`, version
  directories `.sys.v#.`) are never treated as entries of the tree.
- Files are written through XRootD with one writer per file and sequential
  offsets. EOS computes the checksum while storing the file and returns it
  at close. Erasure-coded layouts require this write order (EOS up to 5.5.2
  stores zero parity for files written out of order), so ranges of a file
  are never written in parallel and a target that is an EOS FUSE mount is
  warned about.
- The directory's layout and checksum settings decide how a file is stored;
  the tool passes no layout hints.
- EOS to EOS copies use XRootD third-party copy where available, with
  streaming through the client as the fallback.

## Known limitations

- Path-based operations on a POSIX target are not hardened against someone
  with write access to the target swapping directories for symlinks while
  the run is in progress (the usual TOCTOU of path-based tools). Targets are
  expected to be writable only by the user running eosmirror.
- A `--retry-failed` run that has to recreate a missing parent directory
  does not restore the mtime of that directory's own parent; the next full
  run does.
- Shards sharing a target can finalize a directory's mtime before another
  shard creates a subdirectory in it; the next run fixes it.
- POSIX mtime resolutions coarser than one second (FAT) are treated as one
  second.
- EOS's find output prints names raw, one entry per line. A name with line
  breaks can therefore add lines that look like entries. Lines that cannot be
  parsed and names listed twice are skipped, but a forged line for a name
  that is not otherwise in the directory cannot be told from a real one.

## Not in scope (for now)

Extended attributes, ACLs, hard links (copied as separate files), delta or
partial updates, checksum-based change detection, id mapping between sites.
