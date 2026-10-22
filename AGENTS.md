# Notes for AI agents

Read [README.md](README.md) (what eosmirror does, for users) and
[docs/architecture.md](docs/architecture.md) (how it is built: parts,
threads, the states of a copy, the invariants, the tests) before changing
code. [docs/design.md](docs/design.md) records the decisions behind the
behavior and the EOS and XRootD specifics; read the part that concerns your
change. [AI_POLICY.md](AI_POLICY.md) applies.

## Building and testing

- `scripts/check.sh PLATFORM [SANITIZER]` configures, builds and runs the
  unit tests in a rootless podman container (`el9`, `ubuntu22`, `ubuntu24`;
  `address`, `thread` or `undefined`; images from `scripts/build-images.sh`),
  so that the host needs nothing but podman. `scripts/integration-test.sh
  PLATFORM [SANITIZER]` runs the integration tests of that build against an
  xrootd server and a single-host EOS instance in containers.
  `scripts/in-container.sh PLATFORM ctest --test-dir /build/none -C long -L
  long` runs the long tests. A native build is as in the README.
- Before a commit: the unit tests under the address and thread sanitizers
  for a change to the engine, the pipeline, an endpoint or the journal; the
  integration tests for a change to an endpoint or to `copy.cc`; the long
  tests for a change to the engine, the journal or the local endpoint's
  durability. CI runs the EOS integration and the long tests only nightly
  and on demand. Say which tests ran and which did not.

## Changing code

- Keep the invariants in docs/architecture.md. A change to one is a decision
  for the maintainers, recorded in docs/design.md, not a side effect of a
  fix.
- Errors go through `Result<T>` and `Status`, never exceptions. Operations on
  endpoints are retried only for transient kinds. New `ErrorKind`s and
  `EntryType`s go last: journals store the numbers. A change to the
  journal's tables, or to what its rows mean, raises its version
  (journal.cc).
- Names from listings are untrusted data: validate them before joining them
  into paths, print them through `printable()`, and refuse what a parser
  cannot tell apart rather than guess.
- Style: `.clang-format` (Google style, 100 columns), an SPDX header on every
  source file. Comments say why, and what a caller may rely on; the code
  says what. Prose in comments, docs and help texts is plain and concise.
- Tests: doctest. The engine is tested on `FakeEndpoint`
  (tests/unit/fake_endpoint.hh), which has a switch for each endpoint
  behavior the engine must cope with; a behavior found in a real endpoint
  gets a switch and an engine test. Real servers are tested in
  tests/integration. A change to what the target ends up with needs a test
  of the resulting state, not of the calls made.
- Keep the docs with the code: the README and the `--help` text for what
  users see, docs/design.md for a decision or a limitation,
  docs/architecture.md for structure, state and tests. Rewrite the section
  so that it stays tight, instead of appending.

## Repository hygiene

- The repository is public. Nothing in it may identify an installation: no
  host names, site paths, addresses, credentials, keytabs or the
  configuration of a particular instance, in examples and commit messages
  included. Tests use CERN's public EOS image.
- Commits in sensible units on a branch other than `main`: a subject line
  that says what changed, a body that says why, and the trailer
  `Assisted-By: generative AI` on AI-assisted commits.
- Nothing is reported to CERN or other upstreams without the maintainers'
  instruction.
