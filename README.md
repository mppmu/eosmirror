# eosmirror

One-way replication of large file trees between POSIX file systems, XRootD
servers and [EOS](https://eos-web.web.cern.ch/) instances.

eosmirror copies a tree from a source to a target so that the target ends up
with the same files, symlinks, directories, owners, modes and modification
times. It is made for very large trees over high-latency links: it keeps many
transfers and metadata operations in flight, works directory by directory with
bounded memory, never stops on single errors, and keeps a journal so that a
later run retries exactly what failed.

Status: work in progress, not yet usable. See [docs/design.md](docs/design.md).

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

## License

GPL-3.0-or-later, see [LICENSE](LICENSE). The bundled test framework
[doctest](https://github.com/doctest/doctest) is MIT licensed.
