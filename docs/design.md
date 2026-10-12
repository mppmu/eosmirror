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
  `--delete`, up to `--max-delete` deletions per run. A source directory
  that is empty, against a target directory that is not, is more likely an
  unmounted file system (an automount, say) or a broken server than a tree
  to delete: for the top directory such a run stops without deleting
  anything, below it nothing is deleted in that directory (counted and
  warned about) unless `--delete-emptied` says otherwise. A missing source stops a run before it
  starts, and so do a source and target that overlap (the same tree, or
  one inside the other): the run would copy the target into itself, or
  delete what is around the source. Trees are compared by real path, for a
  target that does not exist yet through its deepest existing ancestor, and
  remote ones by server (host and port) and path, the root of EOS endpoints
  resolved like realpath(3) and that of plain XRootD normalized.
- Files and directories that vanish from the source between the listing of
  their directory and their copy or walk are counted as such, and are no
  failures: the source changed, and the next run finds them gone. They do
  count towards `--max-consecutive-failures`, so that a source that goes
  away as a whole stops the run.

Every copy writes to a temporary name (or uses EOS's atomic upload) and
renames into place, so an interrupted run never leaves a partial file under
the real name. Owner, mode and mtime are applied to the temporary file before
the rename, so a file never appears with wrong metadata either. The target
mtime is always the source mtime as observed *before* reading the file (by a
stat, or for EOS sources from the listing, see below). If the source changed
while it was read (size or mtime differ afterwards), the copy is reported as
"changed" and retried; whatever was renamed into place carries the old mtime,
so a later run copies it again. Temporaries on the target that neither
their mtime nor their ctime shows changed for an hour are taken for
leftovers of interrupted runs and removed with `--delete`; a copy sets the
mtime of its temporary before the rename, and EOS gives an atomic upload
the source's mtime when it creates it, which is why the ctime counts.

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
with a warning and counted, on both sides and when deleting trees. Listings
report the entries they left out themselves (lines of an EOS listing that
cannot be told apart, names a plain XRootD server cannot address). In a
directory where either listing missed entries, `--delete` deletes nothing,
since the other side may list one of them as an entry the source lacks;
such directories are counted and warned about. Log lines,
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
running at once adapts: it starts at `--min-transfers` (4) and every 10 s
rises by half (at least 2) while the throughput in bytes or files beats the
one measured at the previous limit by more than 5 %, up to `--max-transfers`
(64). A rise after which the throughput falls more than 10 % short of that
one, in bytes and in files, did not pay: the limit goes back to where it was
and does not rise for a minute (6 intervals), which keeps it from going back
and forth. The limit falls by a quarter, down to the start value, when copies had
to be retried for errors of the target (opening, writing or committing a
file), which is how an overloaded target shows; retries for a changing or
failing source do not count. The interval after a decrease only measures the
lower limit. The throughput to beat decays by 0.5 % per interval, so that a
record from a faster part of the tree blocks growth only for a while, and a
flat throughput tries a higher limit about every 100 s. A throughput that
neither improves nor drops by more than 10 % after such a try keeps the
higher limit. `--transfers N` runs exactly N copies at once.

The memory of the transfers is mostly their buffers. A copy holds up to
`--read-window` plus `--write-window` plus one buffers of `--buffer-size`
(one side's window plus one where the other side is local), and each
transfer keeps the buffers it allocated for its next copies. At the defaults, 64
transfers hold up to 64 × (4 + 4 + 1) × 8 MiB = 4.5 GiB between two remote
endpoints and 64 × 5 × 8 MiB = 2.5 GiB between EOS and a local file system,
reached only with files of several chunks; small files touch only the start
of their buffer.

### Retries and the journal

Operations are retried with exponential backoff while the error is
transient (I/O errors, timeouts, busy or failing servers, the source
changing). A full disk or exceeded quota is not retried. A retry that
cancellation cuts short is no failure. What still fails is recorded in the
journal (`--journal FILE`, SQLite) with the path, the kind of entry and the
last error, and the run goes on. Finalized directories are recorded too.

An endpoint that becomes unusable during a run (a target outage, an
unmounted source) would make every remaining entry fail and fill the
journal (or vanish one by one). After `--max-consecutive-failures`
failures or vanished entries (default 1000) without a success in between,
the run therefore stops with exit status 4, and the
journal marks it as interrupted, so that `--resume` continues it later.

Modes:

- A plain run with a journal walks everything. Paths that succeed (copied,
  metadata fixed, deleted, vanished from the source, or gone from both sides,
  also with the directory above them) drop out of the failure table, paths
  that fail replace their row.
- Files whose row is a transient error other than a change of the source
  (I/O, timeout, checksum) are copied again by any run with the journal,
  whatever their size and mtime: such a copy may have failed its
  verification or close in place (EOS) and kept the source's size and mtime
  when it could be neither removed nor given mtime 0, which then fails it as
  an I/O error whatever went wrong. Before the new copy, the file gets
  mtime 0, so that an upload that does not arrive cannot be taken for it.
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
bytes, unchanged, metadata fixed, symlinks, skipped special files, entries
vanished from the source, deleted, skipped invalid names, failed by kind,
retries) go to a summary at the end
and optionally to a periodic progress line. So do the times a transfer
found no copy to make while every checker was busy ("listing-bound"),
which shows that listing bounds the run and more `--checkers` may help. Exit status: 0 when everything succeeded, 1 when
something failed after retries (also the metadata of the top directory), 2
for usage errors, 3 when the run could not start (unreadable source,
unusable journal, failed preflight, overlapping source and target, a top
directory that cannot be listed, an empty source against a target that is
not), 4 when it stopped after
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
by a checksum query (where nothing is compared, a stat checks the size),
and the file is renamed into place. A server that refuses to replace an
existing file gets that file moved aside to a temporary name first, removed
once the new one is in place, or moved back if that fails. A file read from a
server that computes checksums is verified by a checksum query after the
read when the target computes none; the server may have to read the file
once more to answer it.

A server executes the requests that come over one connection one after
the other (`Xrd/XrdLinkXeq.cc`: the link's protocol processes a request
before the next is read), and EOS runs a whole find inside the open of its
proc file (`mgm/proc/IProcCommand.cc`). XrdCl would share one connection
per server (user, host and port) among all requests of a process, so each
thread of eosmirror sends its requests (file opens with their reads and
writes, stats, checksum queries, directory operations, listings and MGM
commands) over connections of its own, told apart by the `xrdcl.intent`
parameter, which XrdCl keys connections by (`XrdClURL.cc`,
`GetChannelId`), keeps across redirections and does not send to servers.
Each file still goes over one connection per server. A run thus holds up
to one connection per thread to the MGM and to each storage node it
reads from or writes to, which XrdCl closes after some minutes without
use. `--shared-connection` goes back to one connection per server. On the
single-host test instance, a download of 20,000 small files at 64
transfers took 1.3 s instead of 5.4-5.9 s over one connection (2.1 s with
connections of their own for the file opens only), and listing its 446
directories with 8 checkers 0.32 s instead of 2.4-2.6 s; uploads, bound
by the instance's namespace, took about as long either way.

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
  the mount explicitly. A run logs both endpoints as resolved, with the
  server, the real path and for such paths where the MGM came from, so that
  a stale `EOS_MGM_URL` does not go unnoticed.
- Paths are sent percent-encoded behind `/#curl#` (`/#curl#/eos/a%20b`),
  with `eos.encodepath=1` among the opaque parameters: in opens, in plain
  XRootD requests (mkdir, rm, rmdir, checksum queries, which take
  `<path>?eos.encodepath=1`), in OpaqueFile queries and as `mgm.path` of
  commands. The MGM decodes only paths with this prefix.
- Every request carries `eos.app=eosmirror`, under which EOS's IO statistics
  (`eos io stat -x`) account eosmirror's traffic.
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
  `Format="type,size,uid,gid,mode,flags,mtime,ctime,checksum,checksumtype,link"`,
  plus `SkipVersionDirs`. Each output line is `path="<abs>" type=... size=N
  uid=U gid=G mode=<octal, directories> flags=<octal, files>
  mtime=<sec>.<nsec> ctime=<sec>.<nsec> checksum=<hex, files> checksumtype=<adler, crc32c, ...,
  none> target="<link target>"`, the fields in the order of the format and
  the target only for symlinks (`mgm/proc/user/NewfindCmd.cc`, printFormat).
  A file's checksum is kept with its entry when its type is adler32; one of
  all zeros (a type without a stored value) counts as none.
  The start directory itself is listed too and skipped; a listing without it
  is an error. Paths and targets are printed raw: lines with more than one
  `" type=` (which a name or target can contain), names with control
  characters, entries outside the directory, names listed twice and fields
  that are not numbers where numbers belong are skipped with a warning.
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
  of the source and would pass for a good copy. So is the upload after a
  close that failed, if a file with its size and mtime is in place: the
  storage node may have committed it all the same. Owner and mode are applied
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
  source adds only the open per file, which redirects to a storage node
  (empty files the MGM serves itself), see below.
- A file is read from an EOS source with the metadata of the listing, which
  is exact (nanosecond mtime, owners, mode) and has the checksum. The open
  brings the storage node's stat of the file (XrdCl asks every open for it,
  `kXR_retstat`, `XrdClFileStateHandler.cc`), which has the replica's size
  (the logical size of erasure-coded files) but the replica's own mtime in
  seconds, not the namespace's (`fst/XrdFstOfsFile.cc`, `stat`). While that
  size is the listed one, the copy takes the listed entry, and after the
  read the storage node is asked for the replica's stat again (a request on
  the open file, which XrdCl sends to the storage node): another size or
  mtime means that the file changed while it was read, and the copy is
  retried. That stat is of the replica opened: a file replaced during the
  read is not seen there, its copy gets the old, listed mtime, and the next
  run copies it again. A file with another size than listed has changed
  since the listing: it is stated through the MGM before and after reading,
  so that it is still copied in this run, with its current metadata. A file
  rewritten with its size after the listing shows in a checksum that differs
  from the listed one; a stat through the MGM then tells a change (the copy
  fails as changed, after its retries, and the next run copies it) from
  corrupt data (the MGM still has the listed size and mtime). Where the
  listing has no checksum (a directory without `sys.forced.checksum`), such
  a file is copied with the listed, older mtime, and the next run copies it
  again.
- EOS's own hidden entries (atomic temporaries `.sys.a#.`, version
  directories `.sys.v#.`) are never treated as entries of the tree. An
  atomic temporary gets the `eos.mtime` of its open when EOS creates it,
  and the creation time as ctime (`mgm/ofs/XrdMgmOfsFile.cc`, setCTimeNow
  at the creation, setMTime from `eos.mtime` further down), which the
  storage nodes do not change before the close: an upload in progress for
  longer than an hour is taken for a leftover by a run with `--delete`.
- Files are written through XRootD with up to `--write-window` writes in
  flight (default 4). EOS up to 5.5.2 stores zero parity for an
  erasure-coded file when the first storage node executes a write that does
  not start where the previous one ended. eosmirror therefore writes each
  file through one XrdCl file over one connection, from one thread, each
  write starting where the previous one ended and only the last one short,
  with XrdCl's write recovery off (which would resend writes over a new
  connection). XrdCl sends writes over the first substream however many a
  connection has (it never multiplexes `kXR_write`,
  `XrdClXRootDTransport.cc`), so `XRD_SUBSTREAMSPERCHANNEL` does not
  matter, and sends a connection's requests in the order they were
  submitted (one FIFO per substream, `XrdClStream.cc`). It asks for
  asynchronous I/O in every open (`kXR_async`, `XrdClFileStateHandler.cc`),
  but a storage node running with `xrootd.async off`, which EOS's packaged
  configuration sets (`misc/etc/xrd.cf.fst`, `misc/etc/eos/config/fst/fst`),
  executes them in that order. The client cannot tell a storage node that
  runs with async on. Pipelined writes are thus safe for erasure-coded
  files only while the FSTs keep EOS's default `xrootd.async off`, as are
  xrdcp and `eos rclone`, which keep 4 writes of 8 MiB in flight in the
  same order (`XrdClClassicCopyJob.cc`, `XrdClConstants.hh`). A run with an
  EOS target and a larger window says so at the start; `--write-window 1`
  opts out. Once a write has failed, nothing more is sent; the writes in
  flight are waited for and the upload is discarded. EOS computes the
  checksum while storing the file.
- The integration tests check the parity of pipelined raid6 uploads by
  reading them back with two stripes dropped (`file drop`), so that data
  stripes must be made up from the parity. Setting filesystems' config
  status to off would not do: EOS 5.5.2 still names their stripes to the
  storage node that reads the file (the masking of offline filesystems in
  `mgm/ofs/XrdMgmOfsFile.cc` is overwritten right after it is set).
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
