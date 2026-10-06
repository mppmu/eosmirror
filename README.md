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

`eosmirror sync --help` lists the options: dry runs, deletion of extra
entries with a cap, worker counts, retries, resuming an interrupted run,
sharding a tree over several hosts (`--shard K/N`) and symlink rewriting.
The exit status is 0 when everything succeeded, 1 when some entries failed
after retries, 2 for usage errors, 3 when the run could not start and 130
when interrupted.

## Building

Requires a C++20 compiler (gcc 11 or newer), CMake 3.20 or newer and SQLite 3.
The XRootD client library (XrdCl) is needed for XRootD and EOS endpoints.

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

## License

GPL-3.0-or-later, see [LICENSE](LICENSE). The bundled test framework
[doctest](https://github.com/doctest/doctest) is MIT licensed.
