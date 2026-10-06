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
  `--delete`, up to `--max-delete` deletions per run. A source whose top
  directory is empty, against a target that is not, is more likely an
  unmounted file system or a broken server than a tree to delete: such a run
  stops without deleting anything. A missing source stops a run before it
  starts.

Every copy writes to a temporary name (or uses EOS's atomic upload) and
renames into place, so an interrupted run never leaves a partial file under
the real name. Owner, mode and mtime are applied to the temporary file before
the rename, so a file never appears with wrong metadata either. The target
mtime is always the source mtime as observed *before* reading the file. If
the source changed while it was read (size or mtime differ afterwards), the
copy is reported as "changed" and retried; whatever was renamed into place
carries the old mtime, so a later run copies it again.

Checksums (adler32, through zlib) are computed while streaming and compared
with what an endpoint stores: an EOS or XRootD target with the checksum it
computed while storing the file, and an EOS or XRootD source with the
checksum it stores for the file where the target computes none (a POSIX
target) or where it comes with the listing anyway (EOS).
A mismatch is an error like any other: the copy is discarded and retried.
For a POSIX target, `--verify-readback` reads the written file back too.
`--require-checksum` fails copies that neither side could verify.

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
it on the target, by writing a file there, so never on a source or in a dry
run), whether entries have owners at all, whether owners,
modes (and which mode bits of files and of directories) and mtimes can be
set, whether symlinks exist and have owners, which checksum it computes. The
engine adapts: comparison granularity, size-only comparison without mtimes,
skipped symlinks, masked mode comparison, no owners from a source that has
none (with a warning), which checksum to compute, what the preflight checks.
Without the privilege to set owners, the engine makes read-only target
directories writable (the owner's rwx bits) while it changes their entries
or sets their mtime, which EOS allows only with write access, and restores
their mode afterwards, also in directories that another shard owns.
Read-only directories that `--delete` removes are made writable first. An
endpoint can also warn about being used as a target (a POSIX target on an
EOS FUSE mount, see below).

Listings of endpoints that follow symlinks (plain XRootD servers do) can
lead back into a directory being walked. Such endpoints, and POSIX, give
directories an identity (device and inode); a subdirectory with the identity
of one of its ancestors is reported as a failure and not walked.

`eosmirror selftest TARGET` exercises all of this in a temporary directory
under a target before a long run and reports what the target cannot do.

Endpoint operations are synchronous; concurrency comes from the worker pools
below. Within a file, remote endpoints pipeline: data moves in chunks of
`--buffer-size`, in buffers from a small pool per transfer, which the reader
fills and the writer sends as they are. XRootD and EOS readers keep up to
`--read-window` chunk reads in flight and hand the chunks over in order;
XRootD and EOS writers keep up to `--write-window` writes in flight, each
starting where the previous one ended, and `commit()` waits for them. The
windows bound the buffers a transfer holds (reads plus writes plus one).
Once a read or write has failed, nothing more is sent; the requests in
flight are waited for before a file is closed, since XrdCl refuses to close
a file with requests in flight and may complete them after the close. The
first chunk is read before the target file is created, so that an
unreadable source leaves nothing on the target, and the reads ahead overlap
the target's open.

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
metadata is applied and the parent's counter is decremented. A transfer
releases its slot before it finalizes a directory. The run ends when the
root is finalized and the queues are drained.

Separate pools matter because a tree of tiny files is bound by metadata
latency and a tree of huge files by bandwidth. The number of transfers
running at once adapts: it starts at `--min-transfers` and every 10 s rises
by half (at least 2) while the throughput in bytes or files beats the one
measured at the previous limit by more than 5 %, up to `--transfers`. It
falls by a quarter, down to the start value, when copies had to be retried
for errors of the target (opening, writing or committing a file), which is
how an overloaded target shows; retries for a changing or failing source do
not count. The interval after a decrease only measures the lower limit. The
throughput to beat decays by 0.5 % per interval, so that a record from a
faster part of the tree blocks growth only for a while, and a flat
throughput tries a higher limit about every 100 s.

### Retries and the journal

Operations are retried with exponential backoff while the error is
transient (I/O errors, timeouts, busy or failing servers, the source
changing). A full disk or exceeded quota is not retried. A retry that
cancellation cuts short is no failure. What still fails is recorded in the
journal (`--journal FILE`, SQLite) with the path, the kind of entry and the
last error, and the run goes on. Finalized directories are recorded too.

An endpoint that becomes unusable during a run (a target outage, an
unmounted source) would make every remaining entry fail and fill the
journal. After `--max-consecutive-failures` failures (default 1000) without
a success in between, the run therefore stops with exit status 4, and the
journal marks it as interrupted, so that `--resume` continues it later.

Modes:

- A plain run with a journal walks everything. Paths that succeed (copied,
  metadata fixed, deleted, or gone from both sides, also with the directory
  above them) drop out of the failure table, paths that fail replace their
  row.
- `--resume` skips directories finalized by the previous run if that run
  was interrupted; after a completed run there is nothing to resume and
  everything is walked. Recorded failures are not retried by resuming.
- `--retry-failed` processes only the recorded failures: each within its
  parent directory, failed directories as subtree walks (rows below them are
  covered by that walk), the top directory as a walk of the whole tree, and
  failed deletions with `--delete`. Rows whose directory is gone from the
  source are dropped; rows whose target directory cannot be created again
  (when one above it is gone) are kept, and a plain run recreates it.
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
something failed after retries (also the metadata of the top directory), 2
for usage errors, 3 when the run could not start (unreadable source,
unusable journal, failed preflight, a top directory that cannot be listed,
an empty source against a target that is not), 4 when it stopped after
failures in a row, 130 when interrupted.

## XRootD endpoint

URLs are `root://host[:port]//path`, or `roots://` for TLS. The protocol is
kept for every connection, and opaque parameters of the URL (`?authz=...`,
`xrd.wantprot=...`) go with every request and open, after the request's own
parameters; messages and the journal show the URL without them.

An XRootD URL is first asked for EOS (an MGM answers the `whoami` command).
A server that refuses the command is a plain XRootD server; one that cannot
be reached or refuses the login is an error, never a reason to fall back to
plain XRootD. The same holds for the checksum configuration query of a plain
server. The detected type is logged.

Plain XRootD servers list directories with per-entry stat in one call, store
no symlinks, report no owners, cannot set owners or mtimes, keep only the
permission bits of modes, and compute a checksum only when configured
(`query config chksum` tells which). Their listings and stats follow
symlinks. The endpoint reports these as capabilities; the engine then
compares files by size only, skips symlinks (counted), applies no mtimes,
ignores owners of such a source, and detects directory cycles. Writes go to
a temporary name `.<name>.eosmirror-<12 hex digits>` with up to
`--write-window` writes in flight per file, the stored checksum is verified
by a checksum query, and the file is renamed into place. A file read from a
server that computes checksums is verified by a checksum query after the
read when the target computes none; the server may have to read the file
once more to answer it.

XrdCl shares one connection per server (user, host and port) among all
files. With `--connection-per-transfer`, every transfer thread opens its
files over connections of its own, told apart by the `xrdcl.intent`
parameter, which XrdCl keys connections by, keeps across redirections and
does not send to servers. Each file still goes over one connection per
server.

XRootD has no escaping for paths: the server takes everything after the
first `?` as opaque parameters, in opens as in all other requests. Paths
containing `?` are therefore refused with an error instead of being sent.

Server errors that mean a busy or failing server (`kXR_Overloaded`,
`kXR_ServerError`, `kXR_noReplicas`, `kXR_inProgress` and `kXR_FSError`,
which servers send for errnos without a protocol code such as `EAGAIN`,
`EBUSY` or `ESTALE`, and which XrdCl turns into `ENODEV`) and client errors
of the connection (TLS, handshake, address, exhausted stream ids) are
retried like I/O errors. So is any failed write or close of an upload,
whatever the error, unless space or permission is missing: the target then
holds no file, and copying again is right.

## EOS specifics (milestone 3)

XRootD has no symlink, chown or utimes operations and its stat lacks owners,
modes and link targets. The EOS endpoint uses the MGM's commands, sent the
way the `eos` client sends them (EOS 5.5 source, `console/` and `mgm/proc/`):

- Endpoints are `root://mgm//eos/...` URLs, or paths below `/eos` on the
  command line, which name the instance of `--mgm` or `EOS_MGM_URL` (the eos
  client's convention) and never a local FUSE mount; `file:///eos/...` names
  the mount explicitly.
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
- The root is resolved to its real path once, by stating each path component
  and following symlinks like realpath(3), since find prints entries under
  real paths and limits its depth by the depth of the path it was given. The
  URL as given is kept for messages and the journal.
- Listing: `find` (RequestProto field 5, FindProto) with `Files`,
  `Directories`, `Maxdepth=1` (a oneof, so it must be sent), `Path` and
  `Format="type,size,uid,gid,mode,flags,mtime,checksum,checksumtype,link"`,
  plus `SkipVersionDirs`. Each output line is `path="<abs>" type=... size=N
  uid=U gid=G mode=<octal, directories> flags=<octal, files>
  mtime=<sec>.<nsec> checksum=<hex, files> checksumtype=<adler, crc32c, ...,
  none> target="<link target>"`, the fields in the order of the format and
  the target only for symlinks (`mgm/proc/user/NewfindCmd.cc`, printFormat).
  A file's checksum is kept with its entry when its type is adler32; one of
  all zeros (a type without a stored value) counts as none.
  The start directory itself is listed too and skipped; a listing without it
  is an error. Paths and targets are printed raw: lines with more than one
  `" type=` (which a name or target can contain), names with control
  characters, entries outside the directory and names listed twice are
  skipped with a warning.
- find limits identities other than root and sudoers (EOS 5.5
  `mgm/proc/user/NewfindCmd.cc`): it stops with `E2BIG` after 100000 files,
  counting the files of the listed subdirectories, or 50000 directories
  (access rules can change the limits), and it leaves out subdirectories the
  identity may not read, with `EACCES` and an `error(13): ... directory
  <path>/` line per subdirectory in its error output. A listing cut short is
  completed from a plain XRootD directory listing (names only) with one stat
  per missing entry, which keeps full metadata but is slow (warned about
  once); its lines for names that the plain listing lacks are dropped. Left
  out subdirectories are stated and listed, so that walking into them fails
  for them alone; error output of another form fails the listing.
- Single stat: `mgm.pcmd=stat` as an OpaqueFile query on the path; the reply
  `stat: dev ino mode nlink uid gid rdev size blksize blocks atime mtime
  ctime atime_ns mtime_ns ctime_ns` has nanoseconds.
- Symlink: the protobuf file command (RequestProto field 29, FileProto with
  `md.path` and `symlink` = FileSymlinkProto with `target_path` and
  `force`), which carries path and target as bytes and replaces an existing
  symlink. The MGM turns `#AND#` in the path into `&` and resolves targets
  starting with `fid:`, `fxid:`, `cid:` or `cxid:`, and find could not list a
  target with a line break back, so such symlinks are refused as failures.
- mtime: `eos.mtime=<sec>.<nsec>` on the open URL for new files; otherwise
  the OpaqueFile query `<encoded path>?eos.encodepath=1&mgm.pcmd=utimes
  &tv1_sec=0&tv1_nsec=0&tv2_sec=S&tv2_nsec=<9 digits>`, which works for
  directories and symlinks as well.
- Owner and mode: the OpaqueFile queries `mgm.pcmd=chown&uid=U&gid=G` and
  `mgm.pcmd=chmod&mode=<decimal>` (`mgm/ofs/fsctl/Chown.cc`, `Chmod.cc`),
  one request each, where the proc commands take an open, two reads and a
  close. The fsctl chown follows symlinks and parses ids as signed integers,
  so symlinks and ids above 2^31 - 1 get `mgm.cmd=chown&mgm.chown.option=h
  &mgm.path=...&mgm.chown.owner=uid:gid` (`h`: the entry itself). EOS
  stores only the permission bits of files and clears setuid on directories
  (`mgm/ofs/cmds/Chmod.inc`), so those bits are not compared.
- Only root can set owners: sudoers can set neither the owner of a directory
  nor the group of a file (the latter is silently dropped) and no mode of an
  entry they do not own (`mgm/ofs/cmds/Chown.inc`, `Chmod.inc`). `whoami`
  tells whether the identity is root; a sudoer gets a note in the debug log.
- Writes use EOS's atomic upload (`eos.atomic=1` on the open URL, with
  `eos.mtime`), so EOS itself renames the file into place at close. A
  storage node commits an upload at any regular close, complete or not, and
  discards it only on a client disconnect, a write error or when told to
  delete it (`fst/XrdFstOfsFile.cc`): an aborted upload waits for its writes
  in flight, also after a failed one, sends the fctl `delete` (XrdCl
  `File::Fcntl`) and closes, and the previous file stays as it was.
- After the close, a checksum query on the file tells the type EOS stored
  and its value; they are compared when the type is the one computed while
  copying (adler32). Files without checksums (`none`, the default without
  `sys.forced.checksum`) or with another type (from the directory or a space
  policy) are counted as unverified, and refused with `--require-checksum`.
  A file that does not match, cannot be queried or is refused is removed
  again, or else gets mtime 0, since it already carries the size and mtime
  of the source and would pass for a good copy. Owner and mode are applied
  after the close, since an upload runs under the client's identity: the
  file is briefly visible with the uploader's owner. The mode is passed in
  the open, which gives a new file that mode plus the owner's read and
  write bits (`XrdXrootdXeq.cc` adds them, `XrdMgmOfsFile.cc` stores the
  permission bits), so the chmod is skipped for a file that did not exist
  on the target and whose mode has those bits. A file that replaces another
  is chmodded all the same: with versioning, EOS gives it the old file's
  owner and mode (`mgm/ofs/fsctl/CommitHelper.cc`). The chown is not skipped
  for owners that equal the identity's: a directory's `sys.owner.auth`
  makes EOS create files as the directory's owner, also for root.
- Per copied file, the MGM sees the open (which redirects to a storage
  node), the checksum query after the close (and a stat where it compares
  nothing) and the chown, plus the chmod in the cases above; per created
  directory the mkdir and, when it is finalized, utimes, chown and chmod;
  per listed directory one find (an open, two reads and a close). An EOS
  source adds an open and two stats per file, before and after reading;
  its checksum comes with the listing.
- EOS's own hidden entries (atomic temporaries `.sys.a#.`, version
  directories `.sys.v#.`) are never treated as entries of the tree.
- Files are written through XRootD with up to `--write-window` writes in
  flight (default 4). EOS up to 5.5.2 stores zero parity for an
  erasure-coded file when the first storage node executes a write that does
  not start where the previous one ended. eosmirror therefore writes each
  file through one XrdCl file over one connection, from one thread, each
  write starting where the previous one ended and only the last one short,
  with XrdCl's write recovery off (which would resend writes over a new
  connection) and its substreams at the default of one (writes always go
  over the first). XrdCl sends a connection's requests in the order they
  were submitted (one FIFO per substream, `XrdClStream.cc`), and a storage
  node running with `xrootd.async off`, which EOS's packaged configuration
  sets (`misc/etc/xrd.cf.fst`, `misc/etc/eos/config/fst/fst`), executes
  them in that order. The client cannot tell a storage node that runs with
  async on. Pipelined writes are thus safe for erasure-coded files only
  while the FSTs keep EOS's default `xrootd.async off`; `--write-window 1`
  opts out. Once a write has failed, nothing more is sent; the writes in
  flight are waited for and the upload is discarded. EOS computes the
  checksum while storing the file.
- XrdCl reports the close of a file whose connection is gone as done
  (`XrdClFileStateHandler.cc`, `Close`), while the storage node discards
  the upload. A commit is therefore confirmed: by the checksum query where
  it compares, else by a stat (size and mtime). When the file in place is
  not the upload (it is the previous one, or none), the copy fails with an
  I/O error and is retried; that file is never removed.
- A POSIX target on an EOS FUSE mount (`fuse.eosxd` for the longest mount
  point above the target in `/proc/self/mountinfo`; more cautiously, a FUSE
  mount of unknown kind, or any FUSE file system where that table is
  missing) gets a warning at the start of a run: writing through eosxd is
  slow and breaks erasure-coded files.
- The directory's layout and checksum settings decide how a file is stored;
  the tool passes no layout hints.
- EOS to EOS copies stream through the client; XRootD third-party copy is a
  possible later addition.

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
- Shards running as a user other than root can restore the mode of a
  read-only directory while another shard still changes entries in it.
  Those changes fail, and the next run makes them.
- POSIX mtime resolutions coarser than one second (FAT) are treated as one
  second.
- EOS's find output prints names raw, one entry per line. A name with line
  breaks can therefore add lines that look like entries. Lines that cannot be
  parsed and names listed twice are skipped, but a forged line for a name
  that is not otherwise in the directory cannot be told from a real one. In
  a listing that find cut short, a forged line can also stand in for an
  existing entry whose own line was cut off.
- Directories that find leaves out for lack of permission are recognized by
  the paths in its error output, which are printed raw as well: a name with
  line breaks there fails the whole listing.

## Not in scope (for now)

Extended attributes, ACLs, hard links (copied as separate files), delta or
partial updates, checksum-based change detection, id mapping between sites.
