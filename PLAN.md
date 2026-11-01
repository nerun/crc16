# PLAN.md — crc16 work plan

This file is the original work plan for porting `crc16.py` to a C
command-line tool that computes many CRC-16 variants of a file in a single
pass. All phases except the AArch64 PMULL path were completed on
2026-11-01; the file is kept as the design record. Current guidance for
agents is in `AGENTS.md`, which takes precedence where the two differ.

## 1. Ground rules

- Language: pure ISO C. No C++, no compiler-specific code outside clearly
  guarded optional paths (see SIMD, phase 7).
- Standard: build with `-std=c23` by default, but avoid C23-only features
  (`constexpr`, `[[attributes]]`, `nullptr`, `bool` as keyword, `#embed`,
  `typeof`, digit separators) unless there is a clear gain. The goal is to
  compile cleanly under `-std=c99`/`c11`/`c17`/`c23` and to document the
  lowest standard that works (expected: C99, or C11 if `_Static_assert` /
  `<threads.h>` are adopted). The minimum standard is stated in `README.md`
  and in the man page.
- Everything in English: identifiers, comments, messages, `--help`, man page,
  commit messages.
- Layout required by `README.md`: a `Makefile`, sources in `src/`, all
  build output in `build/` (never commit `build/`).
- `crc16.py` is the primary reference. Its variant table, aliases and
  omissions are authoritative. Its output must be reproducible bit-for-bit.
- `https://github.com/mattsta/crcspeed` is a secondary reference for speed
  techniques. Do not copy code verbatim unless its license notice is kept
  (crcspeed is BSD-3-Clause; `crcspeed.c` is zlib-licensed, Mark Adler).
  Preferred: write original code and credit the technique in comments.
- Project license: `GPL-2.0-or-later` (full text in `LICENSE`). Every code
  file starts with the SPDX line and the standard GPL notice, copyright
  Daniel Dias Rodrigues. Third-party code must be GPL-2.0-or-later
  compatible and keep its own notice; prefer original code.

## 2. What the reference does (crc16.py)

- 31 variants, Rocksoft/RevEng model: `width=16, poly, init, refin, refout,
  xorout` (normal form). In all 31, `refin == refout`.
- Reads the file once in 1 MiB chunks and updates every variant per chunk.
- Prints `Processados N bytes.` followed by `NAME : XXXX` (uppercase hex,
  4 digits). C version: `Processed N bytes.` with the same layout.
- Aliases (document them, accept them as input names):
  - ARC: IBM, LHA, DF-1
  - IBM-3740: CCITT-FALSE, AUTOSAR
  - IBM-SDLC: X-25, ISO-HDLC, ISO-IEC-14443-3-B
  - KERMIT: V-41-LSB, BLUETOOTH
  - MAXIM-DOW: MAXIM
  - SPI-FUJITSU: AUG-CCITT
  - UMTS: BUYPASS, VERIFONE
  - XMODEM: ACORN, LTE
  - GENIBUS: EPC, EPC-C1G2, I-CODE
  - PROFIBUS: IEC-61158-2
- Deliberately omitted (keep omitted, mention in man page): Fletcher (not a
  CRC), OpenPGP and CAN-FD (not CRC-16), SICK (different algorithm), EFI,
  G-3, EVEREST, ADCCP (no validated specification).

### Check values (CRC of ASCII "123456789")

Verified by running `crc16.py`; they match the RevEng catalogue. These are
the first unit tests.

| Variant | Check | Variant | Check |
|---|---|---|---|
| ARC | BB3D | MCRF4XX | 6F91 |
| CDMA2000 | 4C06 | MODBUS | 4B37 |
| CMS | AEE7 | NRSC-5 | A066 |
| DDS-110 | 9ECF | OPENSAFETY-A | 5D38 |
| DECT-R | 007E | OPENSAFETY-B | 20FE |
| DECT-X | 007F | PROFIBUS | A819 |
| DNP | EA82 | RIELLO | 63D0 |
| EN-13757 | C2B7 | SPI-FUJITSU | E5CC |
| GENIBUS | D64E | T10-DIF | D0DB |
| GSM | CE3C | TELEDISK | 0FB3 |
| IBM-3740 | 29B1 | TMS37157 | 26B1 |
| IBM-SDLC | 906E | UMTS | FEE8 |
| ISO-IEC-14443-3-A | BF05 | USB | B4C8 |
| KERMIT | 2189 | XMODEM | 31C3 |
| LJ1200 | BDF4 | | |
| M17 | 772B | | |
| MAXIM-DOW | 44C2 | | |

Empty input: result is `init` (reflected when `refin`) XOR `xorout`.

## 3. Key design decisions

### 3.1 Reflected variants: use LSB-first tables, not byte reflection

The Python code reflects every input byte and keeps an MSB-first register.
In C, use two native engines instead:

- normal (MSB-first) engine with table built from `poly`;
- reflected (LSB-first) engine with table built from `reflect16(poly)` and
  register initialised with `reflect16(init)`.

Because `refin == refout` for every supported variant, no final reflection
is needed: result = register XOR xorout. Keep a generic bitwise reference
implementation (handles `refin != refout`) for tests only.

### 3.2 Share work between variants (linearity)

A CRC register is linear over GF(2):
`reg(init, data) = reg(0, data) XOR (init · x^(8·len) mod P)`.

Variants that share `(poly, refin)` differ only in `init`/`xorout`, so each
group needs one pass over the data plus a cheap fix-up per member at the
end (`x^(8·len) mod P` via square-and-multiply, O(log len)). The 31
variants collapse into 15 groups:

| Group (poly, reflected?) | Members |
|---|---|
| 0x8005, refl | ARC, MAXIM-DOW, MODBUS, USB |
| 0x8005, normal | CMS, DDS-110, UMTS |
| 0x1021, normal | GENIBUS, GSM, IBM-3740, SPI-FUJITSU, XMODEM |
| 0x1021, refl | IBM-SDLC, ISO-IEC-14443-3-A, KERMIT, MCRF4XX, RIELLO, TMS37157 |
| 0x0589, normal | DECT-R, DECT-X |
| 0x5935, normal | M17, OPENSAFETY-A |
| 0x3D65, refl | DNP |
| 0x3D65, normal | EN-13757 |
| 0xC867, normal | CDMA2000 |
| 0x6F63, normal | LJ1200 |
| 0x080B, refl | NRSC-5 |
| 0x755B, normal | OPENSAFETY-B |
| 0x1DCF, normal | PROFIBUS |
| 0x8BB7, normal | T10-DIF |
| 0xA097, normal | TELEDISK |

Groups must be derived at startup from the variant table (not hard-coded),
so adding a variant needs only a table row. When only some variants are
selected (`-a`), compute only the groups needed.

The same machinery gives CRC combine (zlib `crc32_combine` style), which
enables parallel chunk processing later (phase 8).

### 3.3 Per-group engine tiers

1. Bitwise (reference, tests only).
2. Sarwate: one 256-entry table per group (512 B).
3. Slicing-by-8: 8×256 tables per group (4 KiB; 60 KiB for all 15 groups,
   fits L2). Technique from crcspeed / Mark Adler. Fix crcspeed's flaws:
   - load 8-byte words with `memcpy` (no `*(uint64_t *)p` — strict
     aliasing and alignment UB);
   - table bootstrap must not pass `&int` as a byte (crcspeed's
     `crcfn(0, &n, 1)` is endian-dependent);
   - handle big endian explicitly (byte-wise assembly of the word, or a
     separately generated table set) and test it.
4. Optional slicing-by-16 if benchmarks show a win.
5. Optional carry-less multiply folding (phase 7).

Tables are generated at runtime at startup (microseconds), or optionally
precomputed by a small generator program run by `make`. Pick one and keep
it simple; runtime generation is the default.

### 3.4 I/O and cache strategy

- Read with `fread` into a large aligned buffer (default 1 MiB, tunable).
  Inside the buffer, process in sub-blocks (e.g. 32–64 KiB) and run every
  active group over each sub-block, so data stays hot in L1/L2 while
  tables rotate. Benchmark sub-block size.
- Byte counter is `uint64_t` (files larger than 4 GiB).
- Support stdin (no file argument, or `-`) and multiple files.
- Use `setvbuf(fp, NULL, _IONBF, 0)` to avoid double buffering.
- POSIX extras (`posix_fadvise`, `open`/`read`) only if benchmarks justify
  them, behind `#if defined(_POSIX_VERSION)` with an ISO fallback.

## 4. Proposed source layout

```
Makefile
README.md
AGENTS.md
PLAN.md
LICENSE
crc16.py              # reference implementation (kept)
man/crc16.1           # GNU roff man page (man macros, tbl)
src/
  crc16.h             # public types: crc16_variant, crc16_group, API
  variants.c          # variant table: name, aliases, params, check value
  engine.c            # table generation, Sarwate, slicing-by-8 (normal/refl)
  gf2.c               # reflect16, mulmod, x^n mod P, combine
  multi.c             # group building, multi-variant streaming context
  cli.c               # argument parsing (own parser, no getopt_long)
  output.c            # text / csv / json? output formatters
  main.c              # entry point, file loop, exit codes
  simd_x86.c          # optional, phase 7
  simd_arm.c          # optional, phase 7
tests/
  test_main.c         # unit tests (check values, residues, combine, chunks)
  ref_compare.sh      # compare against crc16.py on random files
bench/
  bench.c             # throughput per engine and per sub-block size
build/                # generated, ignored
```

Streaming API (allows any chunking, needed for stdin and tests):

```c
typedef struct crc16_ctx crc16_ctx;
int  crc16_ctx_init(crc16_ctx *ctx, const size_t *variant_ids, size_t n);
void crc16_ctx_update(crc16_ctx *ctx, const void *data, size_t len);
void crc16_ctx_final(const crc16_ctx *ctx, uint16_t *results);
```

## 5. Command-line interface

```
crc16 [OPTION]... [FILE]...
```

| Option | Meaning |
|---|---|
| `-a, --algorithm NAME` | Select variant(s); repeatable or comma list; case-insensitive; accepts aliases and names with or without `CRC-16/` |
| `-l, --list` | List variants: name, poly, init, refin, refout, xorout, check, aliases |
| `-f, --format FMT` | `text` (default, Python-compatible), `csv`, `tsv`, `json` (optional) |
| `-x, --lowercase` | Lowercase hex digits |
| `-q, --quiet` | Omit the `Processed N bytes.` line |
| `--self-test` | Verify every variant against its check value; exit 0/1 |
| `-h, --help` | Usage |
| `-V, --version` | Version and minimum ISO C standard |

- No file or `-` reads stdin. Multiple files: one block per file, headed
  by the file name.
- Exit status: 0 success, 1 at least one file failed (continue with the
  rest), 2 usage error. On SIGINT the default termination is acceptable.
- Errors go to stderr as `crc16: FILE: strerror(errno)`.
- Argument parsing is hand-written (getopt_long is not ISO C).

## 6. Phases and acceptance criteria

### Phase 0 — scaffolding
- `Makefile`, `src/`, `build/`, `.gitignore` (`build/`), `git init` if the
  owner wants it.
- Makefile (GNU make) targets: `all` (default), `clean`, `check`, `bench`,
  `install`, `uninstall`, `man` (lint), `std-check`, `sanitize`.
- Variables: `CC`, `STD ?= c23`, `CFLAGS ?= -O2`, `CPPFLAGS`, `LDFLAGS`,
  `PREFIX ?= /usr/local`, `DESTDIR`, `BINDIR`, `MANDIR`.
- Warnings: `-Wall -Wextra -Wpedantic -Wconversion -Wshadow
  -Wstrict-prototypes -Wmissing-prototypes`; `-Werror` only in CI-like
  targets. Automatic dependencies with `-MMD -MP`.
- Done when `make` builds an empty `build/crc16` with zero warnings.

### Phase 1 — correct reference core
- `variants.c` with all 31 rows, aliases and check values.
- Bitwise generic implementation + Sarwate normal/reflected engines.
- `crc16_ctx` streaming API using Sarwate, one state per variant (no
  grouping yet).
- `tests/test_main.c`: all check values, empty input, same result for any
  chunk split (random splits of a random buffer), bitwise == table.
- Done when `make check` passes.

### Phase 2 — CLI and output
- `cli.c`, `output.c`, `main.c` as in section 5.
- `tests/ref_compare.sh`: random files (0 B, 1 B, 7 B, 1 MiB−1, 1 MiB,
  1 MiB+1, ~20 MiB) compared byte-for-byte with `python3 crc16.py`
  output (after translating the header line). Skipped if no `python3`.
- Done when outputs match and all error paths are tested (missing file,
  directory, unreadable file, bad option, unknown algorithm).

### Phase 3 — grouping by (poly, refin)
- `gf2.c`: `mulmod`, `xpow8n_mod` (square-and-multiply), normal and
  reflected domains.
- `multi.c`: build groups at init, one register per group, per-member
  fix-up in `final`.
- Tests: grouped result == ungrouped result for all variants and many
  lengths, including 0 and lengths > 2^32 simulated via the math
  functions directly.

### Phase 4 — slicing-by-8
- Normal and reflected slicing-by-8 per group, `memcpy` loads, little and
  big endian paths.
- Tests: identical to Sarwate for all alignments (offset 0–15) and lengths
  0–64 plus large buffers. Run with `make sanitize`
  (`-fsanitize=address,undefined`).
- Big-endian check: `qemu-user` + cross gcc (e.g. `s390x` or `powerpc64`)
  if available; otherwise a forced "generic byte assembly" build flag
  that is exercised on little endian too.

### Phase 5 — benchmarking and tuning
- `bench/bench.c`: MB/s per engine, per group count, per sub-block size,
  on buffers in memory (exclude disk I/O).
- Compare against `crc16.py` and `cksum`, record numbers in README.
- Tune sub-block size and decide on slicing-by-16. Keep changes only when
  they are measurable.

### Phase 6 — man page and docs
- `man/crc16.1`, GNU roff with `man` macros (`'\" t` first line if tbl is
  used). Sections: NAME, SYNOPSIS, DESCRIPTION, OPTIONS, ALGORITHMS (table
  of variants, parameters, check values, aliases), OUTPUT FORMATS, EXIT
  STATUS, EXAMPLES, STANDARDS (minimum ISO C), NOTES (omitted variants and
  why), SEE ALSO (`cksum(1)`, `sha256sum(1)`), AUTHORS, BUGS.
- Lint: `groff -t -man -Tutf8 -ww -z man/crc16.1` (and `mandoc -Tlint` if
  installed). `make install` installs it to `$(MANDIR)/man1`.
- Update README (English): build, usage, minimum standard, benchmarks,
  credits.

### Phase 7 — optional SIMD (carry-less multiply folding)
- Generic folding for any 16-bit poly, normal and reflected, with constants
  (`x^k mod P`, Barrett `mu`) computed at runtime from `poly`, following
  Intel's "Fast CRC Computation for Generic Polynomials Using PCLMULQDQ"
  and the Linux kernel CRC template (`crc-pclmul-template.S`, constants
  from `scripts/gen-crc-consts.py`). ISA-L `crc16_t10dif_*` treats CRC-16
  as a scaled 32-bit CRC; that trick works for every poly.
- x86-64 PCLMULQDQ and AArch64 PMULL, guarded by `#if` and runtime CPU
  detection; enabled with `make SIMD=1`. The portable path stays default
  and is always tested.
- Note: crcspeed's own x86 CRC-16 SIMD falls back to slicing-by-8 in
  several paths; use it only as inspiration.
- Done when SIMD results equal portable results on all tests and the bench
  shows a clear gain.

### Phase 8 — optional parallelism
- One worker per group, or chunk-parallel with CRC combine.
- C11 `<threads.h>` when `__STDC_NO_THREADS__` is not defined, otherwise
  single-threaded. Option `-j N`.
- Only if phase 5 shows the tool is CPU-bound on fast storage.

## 7. Testing checklist (run before every commit)

- `make clean all` with zero warnings under gcc (and clang if available).
- `make check` (unit tests + reference comparison).
- `make sanitize check`.
- `make std-check`: build with `-std=c99`, `c11`, `c17`, `c23` and
  `-pedantic-errors`; record the lowest that passes.
- `make man` (lint) when the man page changes.

## 8. References

- Primary: `crc16.py` (this repository).
- crcspeed (slicing-by-8, SIMD): https://github.com/mattsta/crcspeed
- crcany, Mark Adler (any RevEng CRC, word-wise tables, combine, code
  generation): https://github.com/madler/crcany
- CRC RevEng catalogue, CRC-16 section:
  https://reveng.sourceforge.io/crc-catalogue/16.htm
- Ross Williams, "A Painless Guide to CRC Error Detection Algorithms":
  http://www.zlib.net/crc_v3.txt
- Intel, "Fast CRC Computation for Generic Polynomials Using PCLMULQDQ
  Instruction" (white paper).
- Linux kernel generic PCLMUL CRC template (CRC-T10DIF, 2025):
  https://lkml.iu.edu/hypermail/linux/kernel/2411.3/01103.html
- Intel ISA-L `crc16_t10dif_by4.asm` (CRC-16 folding as scaled CRC-32).
- zlib `crc32_combine` (GF(2) `x^n mod P` technique).
