# AGENTS.md — crc16

Guidance for agents working on this repository. The project is feature
complete; work from here on is maintenance, optional extensions and
performance tuning. The original design and its rationale are in
[`PLAN.md`](PLAN.md) (work plan, phases 0-8). Read it before changing the
architecture; this file wins where the two differ.

## 1. What this is

`crc16` is a C command-line tool that computes 31 CRC-16 variants
(Rocksoft/RevEng model) of files or stdin in a single pass. It ports
`crc16.py`, and its text output must match that script bit for bit (except
that the first line is in English). The main goal is raw speed. On a 6-core
x86-64 it does all 31 variants of a 1 GiB file in about 0.2 s.

## 2. Ground rules

- Pure ISO C, with no C++. Compiler-specific code is allowed only in guarded
  optional paths (`simd_x86.c`, which has a portable fallback).
- Minimum standard: C11 (for `<threads.h>`). The code must still build as C99,
  where it becomes single-threaded through the stub in `parallel.c`. Build
  with `STD=c23` by default but avoid C23-only features. Static assertions
  use the C99 negative-array-size typedef trick (see `variants.c`).
- Everything is in English: identifiers, comments, messages, `--help`, the
  man page and commit messages.
- License: `GPL-2.0-or-later`. Every code file starts with the SPDX line and
  the standard GPL notice, copyright Daniel Dias Rodrigues. Third-party code
  must be compatible and keep its notice; prefer original code.
- `crc16.py` is the authoritative reference for the variant table, aliases
  and omissions. Do not edit it.
- All build output goes to `build/`, which is never committed.

## 3. Layout

```
Makefile              GNU make; all output in build/
README.md             user documentation, benchmarks, credits
AGENTS.md             this file
PLAN.md               original work plan (design record)
LICENSE               GPL-2.0 text
crc16.py              reference implementation (do not modify)
man/crc16.1           man page (man macros + tbl)
src/crc16.h           public types and API
src/variants.c        variant table, aliases, name lookup
src/engine.c          Sarwate + slicing-by-8 (normal/reflected), bitwise reference
src/gf2.c             reflect16, mulmod, x^n mod P
src/multi.c           groups by (poly, refin), streaming context, final fix-up
src/simd_x86.c        PCLMULQDQ folding, runtime CPU detection
src/parallel.c        C11 thread pool, chunk-parallel update + CRC combine
src/cli.c, cli.h      hand-written argument parser (no getopt_long)
src/output.c, .h      text / csv / tsv / json formatters
src/main.c            file loop, mmap/fread I/O, self-test, exit codes
tests/test_main.c     unit tests (~186k checks)
tests/ref_compare.sh  CLI tests and byte-for-byte comparison with crc16.py
bench/bench.c         in-memory throughput per variant count / thread count
```

## 4. Architecture invariants

- **Groups.** Variants that share `(poly, refin)` share one register that
  starts at zero. Each member is fixed up in `crc16_ctx_final` with
  `init * x^(8*len) mod P`. Groups are derived at init from the table, so
  adding a variant needs only a row in `variants.c` (plus its check value)
  and updates to the docs.
- **refin == refout** holds for every supported variant, and the fast path
  relies on it (`crc16_ctx_init` rejects other variants). Supporting one
  would need an output reflection step.
- **Engines.** Reflected variants use LSB-first tables, so input bytes are
  never reflected. SIMD (lengths of 64 bytes or more) and slicing-by-8 must
  give the same result as the byte-wise Sarwate engine and as
  `crc16_bitwise`.
- **Context reuse.** `main.c` builds the context once per run and calls
  `crc16_ctx_reset` per file. Do not rebuild the tables per file, which cost
  about 60 µs each.
- **Threads.** The pool hands out pieces (128 KiB by default) and merges
  them with the CRC combine identity. The result must never depend on `-j`
  or on the piece size.
- **I/O.** Regular files of 4 MiB or more are mapped with `mmap` in 1 GiB
  windows. Everything else goes through `fread` with two 4 MiB buffers,
  reading one while the pool works on the other.
- **Fixed limits.** `CRC16_MAX_VARIANTS` (64) sizes the arrays. A static
  assertion in `variants.c` stops the build if the table outgrows it.

## 5. Build and test

```
make                 # build/crc16
make check           # unit tests + tests/ref_compare.sh (python3 optional)
make check-portable  # SIMD=0
make check-generic   # -DCRC16_GENERIC_LOAD (big-endian load path)
make sanitize        # ASan + UBSan, -Werror
make tsan            # ThreadSanitizer (pthread shim, -DCRC16_TSAN_PTHREAD)
make std-check       # -std=c99/c11/c17/c23 -pedantic-errors -Werror
make man             # groff lint (and mandoc, if installed)
make bench           # in-memory throughput
```

Before declaring any change done, run all of the following, with zero
warnings and zero failures:

- `make clean all`
- `make check check-portable check-generic sanitize tsan std-check`
- `make BUILD=build/clang CC=clang EXTRA_CFLAGS=-Werror check`
- `make BUILD=build/nt STD=c99 check` (the single-threaded build)
- `make man` whenever `man/crc16.1` changes

Every behaviour change needs a test: a unit test in `tests/test_main.c` for
library code, or a case in `tests/ref_compare.sh` for the CLI. Mutation
checks (deliberately breaking a copy of the code in a scratch directory)
are a good way to show that a new test can actually fail.

## 6. Performance work

- Keep a change only when it is measurable. Use `make bench` for the
  engines, and also time the real CLI on a 1 GiB file in the page cache
  (best of several runs, or the mean of about 20).
- `make bench` uses pre-faulted memory and can disagree with the CLI. For
  example, it prefers 32-64 KiB pieces, while the CLI on mapped files is
  2-4% faster with 128 KiB. Decide with the CLI numbers.
- Update the Performance section of `README.md` when the numbers change.

## 7. Documentation

User-facing behaviour is documented in three places, which must stay in
sync: `--help` (`cli_usage` in `cli.c`), `man/crc16.1` and `README.md`.
Options, exit status, limits and known limitations appear in all three
where relevant.

## 8. Open items

- AArch64 PMULL folding (phase 7 of the plan). Not implemented: no ARM
  hardware to test on. It must go behind `#if` with runtime detection, and
  the tests must compare it with slicing-by-8.
- No real big-endian host has been tested. Only the forced generic load
  path (`check-generic`) has run; a `qemu-user` cross test would close the
  gap.
- `-j 0` counts online CPUs (`sysconf`) and ignores affinity masks and
  cgroup quotas. This is documented.
- TSV and JSON write file names byte for byte, so TSV does not escape tabs
  or newlines, and JSON is valid only for UTF-8 names. This is documented.

## 9. History

All on 2026-11-01:

- The work plan (now `PLAN.md`) was written by Claude Opus 5.5 at high
  effort.
- The plan was implemented (phases 0-8, except AArch64 PMULL) by Claude
  Sonnet 5.5 at medium effort.
- The full project was reviewed by Claude Opus 5.5 at high effort. The
  review:
  - extended `--self-test` to cover the SIMD path and the thread pool;
  - added `crc16_ctx_reset`, so tables are built once per run;
  - fixed `setvbuf` on a stdin that is read more than once;
  - added large-file support for 32-bit systems;
  - added a static assertion for the variant limit;
  - made the bench compare all results;
  - added CLI tests;
  - aligned `README.md`, the man page and `--help` with the code.
