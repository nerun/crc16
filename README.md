# crc16

A fast command-line tool that computes **31 CRC-16 variants** of a file (or
standard input) in a single pass. It is a C port of the reference script
`crc16.py`, and its text output matches that script bit for bit.

```
$ printf 123456789 > demo.txt
$ crc16 -q -a modbus,x-25,xmodem demo.txt
CRC-16/MODBUS            : 4B37
CRC-16/IBM-SDLC          : 906E
CRC-16/XMODEM            : 31C3
```

On a 6-core desktop CPU it processes a 1 GiB file with all 31 variants in
about 0.2 s (roughly 5.6 GB/s), where the Python reference needs about 90 s
for 64 MiB. See [Performance](#performance).

## Features

- 31 variants of the Rocksoft/RevEng model, with their aliases (`x-25`,
  `ccitt-false`, `modbus`, ...). Names are case-insensitive and the
  `CRC-16/` prefix is optional.
- One read of the input for any number of variants; stdin and several files
  are supported.
- Text (Python-compatible), CSV, TSV and JSON output.
- Multithreaded, with carry-less multiply (PCLMULQDQ) folding on x86-64 and
  a portable slicing-by-8 fallback everywhere else.
- Memory-maps large regular files; pipes and small files use `fread`.
- Self-test against the published check values (`crc16 --self-test`).

## Building

Requirements: GNU make and a C compiler (tested with gcc 16 and clang 21).

```
make              # builds build/crc16
make check        # unit tests + CLI tests + comparison with crc16.py
                  # (the comparison is skipped when python3 is missing)
make install      # PREFIX=/usr/local; honours DESTDIR, BINDIR and MANDIR
make deb          # Debian packages in build/deb/ (needs dpkg-dev, debhelper)
```

Useful variables: `CC`, `CFLAGS`, `STD` (default `c23`), `SIMD=0` (portable
code only, no PCLMULQDQ), `EXTRA_CFLAGS=-DCRC16_NO_THREADS` (single-threaded
build).

Other targets: `bench`, `man` (lints the man page with groff), `std-check`,
`sanitize` (ASan + UBSan), `tsan` (ThreadSanitizer), `check-portable`,
`check-generic`, `clean`, `uninstall`. All build output goes to `build/`.

`make deb` builds the binary package (plus `-dbgsym`) and the source package
from the `debian/` directory, in a scratch copy under `build/deb/`. The
upstream version in `debian/changelog` must match `CRC16_VERSION`. Pass
`DEB_OPTS=-b` for a binary-only build or `DEB_OPTS=` to sign the results.

### Minimum ISO C standard

The code is pure ISO C. It builds warning-free with `-std=c99`, `c11`,
`c17` and `c23` under `-pedantic-errors`, and uses no C23-only feature.

- **C11** is the minimum for the normal, multithreaded build (it uses
  `<threads.h>`).
- **C99** is enough when threads are unavailable: the build falls back to a
  single-threaded stub when compiled as C99, if `<threads.h>` is missing,
  `__STDC_NO_THREADS__` is defined, or `-DCRC16_NO_THREADS` is given.

POSIX interfaces (`mmap`, `sysconf`) are used only when available, with ISO
fallbacks. The x86-64 SIMD path is chosen at run time from the CPU features.

## Usage

```
crc16 [OPTION]... [FILE]...
```

With no `FILE`, or when `FILE` is `-`, standard input is read.

| Option | Meaning |
|---|---|
| `-a, --algorithm NAME` | Select variants (repeatable or comma list; aliases accepted) |
| `-l, --list` | List variants: parameters, check values, aliases |
| `-f, --format FMT` | `text` (default), `csv`, `tsv`, `json` |
| `-x, --lowercase` | Lowercase hexadecimal digits |
| `-q, --quiet` | Omit the `Processed N bytes.` line |
| `-j, --jobs N` | Worker threads (`0` = one per CPU, the default; `1` = single-threaded; at most 1024) |
| `--no-mmap` | Read files with `fread` instead of mapping them |
| `--self-test` | Check every variant against its check value, and the fast paths (SIMD, threads) against the bit-by-bit reference |
| `-h, --help`, `-V, --version` | Usage; version and minimum ISO C standard |

Exit status: `0` success, `1` at least one input failed (the others are still
processed), `2` usage error. Errors are reported as `crc16: FILE: message`.

The full documentation, including the variant table, is in the manual page:
`man crc16` (source: `man/crc16.1`).

### Variants

ARC, CDMA2000, CMS, DDS-110, DECT-R, DECT-X, DNP, EN-13757, GENIBUS, GSM,
IBM-3740, IBM-SDLC, ISO-IEC-14443-3-A, KERMIT, LJ1200, M17, MAXIM-DOW,
MCRF4XX, MODBUS, NRSC-5, OPENSAFETY-A, OPENSAFETY-B, PROFIBUS, RIELLO,
SPI-FUJITSU, T10-DIF, TELEDISK, TMS37157, UMTS, USB, XMODEM.

Aliases: ARC (IBM, LHA, DF-1); IBM-3740 (CCITT-FALSE, AUTOSAR); IBM-SDLC
(X-25, ISO-HDLC, ISO-IEC-14443-3-B); KERMIT (V-41-LSB, BLUETOOTH);
MAXIM-DOW (MAXIM); SPI-FUJITSU (AUG-CCITT); UMTS (BUYPASS, VERIFONE);
XMODEM (ACORN, LTE); GENIBUS (EPC, EPC-C1G2, I-CODE); PROFIBUS
(IEC-61158-2). Run `crc16 --list` for parameters and check values.

Deliberately omitted: Fletcher (not a CRC), OpenPGP and CAN-FD (not CRC-16),
SICK (a different algorithm), and EFI, G-3, EVEREST and ADCCP (no validated
specification).

## How it works

1. **Groups.** A CRC register is linear over GF(2), so variants with the
   same polynomial and reflection differ only in their initial value and
   final XOR. The 31 variants collapse into 15 groups; each group makes one
   pass over the data with a zero register, and each variant is fixed up at
   the end with `init * x^(8*len) mod P` (square-and-multiply).
2. **Engines per group.** Reflected variants use LSB-first tables and normal
   ones MSB-first tables, so input bytes are never reflected. The bulk of the
   data goes through carry-less multiply folding (x86-64 with PCLMULQDQ), or
   slicing-by-8 tables otherwise; a byte-wise engine and a bit-by-bit
   reference are used by the tests.
3. **Threads.** Large inputs are cut into pieces; each task runs every group
   over one piece, and the partial registers are merged in order with the CRC
   combine identity. Reading the next block overlaps the computation.
4. **I/O.** Big regular files are memory-mapped, so workers read straight
   from the page cache. Copying into a buffer first made the tool I/O-bound:
   it forced a second pass over cold memory.

## Performance

Measured on an AMD Ryzen 5 5500 (6 cores, 12 threads), gcc 16, `-O2`, with a
1 GiB random file in the page cache. Times are the best of five runs.

| Command | Time | Throughput |
|---|---|---|
| `crc16 -j 1 --no-mmap` (31 variants, 1 thread, `fread`) | 1.29 s | 0.83 GB/s |
| `crc16 -j 1` (31 variants, 1 thread, `mmap`) | 1.04 s | 1.0 GB/s |
| `crc16 --no-mmap` (31 variants, 12 threads) | 0.41 s | 2.6 GB/s |
| `crc16` (31 variants, 12 threads, `mmap`) | 0.19 s | 5.6 GB/s |
| `crc16 -a ARC -j 1` (one variant, 1 thread) | 0.14 s | 7.7 GB/s |
| `crc16 -a ARC` (one variant, 12 threads) | 0.05 s | 21 GB/s |
| `cksum` (one CRC-32, for scale) | 0.12 s | 8.9 GB/s |
| `python3 crc16.py` (31 variants, 64 MiB) | 87.5 s | 0.77 MB/s |

In-memory throughput of the library with all 31 variants (15 groups), as
measured by `make bench`:

| Engine | 1 thread | 12 threads |
|---|---|---|
| Slicing-by-8 (`make SIMD=0`) | 137 MB/s | 1.1 GB/s |
| PCLMULQDQ folding | 1.1 GB/s | 5.5-6.0 GB/s |

Tuning notes: thread tasks use 128 KiB pieces. `make bench` (pre-faulted
memory) prefers 32-64 KiB, but the CLI on mapped files is 2-4% faster with
128 KiB, and 256 KiB loses about 15% in both. More than about 10 threads
does not help on this machine; the 4 MiB read buffer only matters for pipes
and small files. Tables are built once per run, so many small files cost
about 20 µs each.

## Testing

`make check` runs about 185,000 unit checks and a comparison script:

- every variant against its check value, for the bit-by-bit reference, the
  table engines and the grouped context;
- empty input, random chunk splits, all alignments and lengths;
- SIMD against slicing-by-8, and threaded against serial results for many
  piece sizes and thread counts;
- GF(2) arithmetic, including lengths beyond 2^32;
- the output compared with `crc16.py` on files from 0 B to 20 MB, mmap,
  `fread` and pipe input around buffer boundaries, all output formats,
  several files and repeated stdin, and the CLI error paths (missing file,
  directory, unreadable file, bad options and arguments).

Before committing, also run `make check-portable check-generic sanitize tsan
std-check`. `check-generic` forces the endian-independent byte assembly used
on big-endian hosts; no real big-endian machine has been tested.

## Limitations

- The SIMD path is x86-64 only. An AArch64 PMULL path is not implemented.
- A file truncated by another process while it is mapped can raise `SIGBUS`;
  use `--no-mmap` to avoid it.
- Only variants with equal input and output reflection are supported.
- File names are written byte for byte: TSV does not escape tabs or
  newlines, and JSON is only valid for UTF-8 names.
- `-j 0` counts online CPUs and ignores CPU affinity masks and cgroup quotas;
  pass `-j N` explicitly in containers.

## Credits

- `crc16.py` is the authoritative reference for the variant table.
- Slicing-by-8 follows the technique of Mark Adler (zlib) as used in
  [crcspeed](https://github.com/mattsta/crcspeed); the code here is original
  and fixes crcspeed's aliasing and endianness pitfalls.
- Carry-less multiply folding follows Intel's white paper "Fast CRC
  Computation for Generic Polynomials Using PCLMULQDQ Instruction".
- Check values and parameters come from the
  [CRC RevEng catalogue](https://reveng.sourceforge.io/crc-catalogue/16.htm).

## License

Copyright (C) 2026 Daniel Dias Rodrigues <danieldiasr@gmail.com>

crc16 is free software, licensed under the GNU General Public License,
version 2 or (at your option) any later version
(`GPL-2.0-or-later`). The full text is in [`LICENSE`](LICENSE); every source
file carries the standard notice and an SPDX identifier.
