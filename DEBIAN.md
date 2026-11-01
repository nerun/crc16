# Debian packaging

How `make deb` builds the Debian packages of crc16, what each output is for,
which standards the packaging follows, and how to maintain it.

## Quick start

```
make deb                  # unsigned source + binary packages in build/deb/
make deb DEB_OPTS=-b      # binary packages only
make deb DEB_OPTS=        # let dpkg-buildpackage sign the results
lintian --pedantic build/deb/*.changes
sudo apt install ./build/deb/crc16_1.0.0-1_amd64.deb
```

Build requirements: `dpkg-dev`, `debhelper` (14 or newer), `fakeroot` is not
needed, and a C compiler. `lintian` is optional but recommended.

## Layout

The `debian/` directory is part of the repository and is edited by hand.
`make deb` never generates or modifies it.

| File | Purpose |
|------|---------|
| `debian/control` | Source and binary package metadata, build dependencies, description |
| `debian/rules` | Build recipe (a `dh` sequencer makefile) |
| `debian/changelog` | Release history; the top entry defines the package version |
| `debian/copyright` | License and copyright, machine-readable format 1.0 |
| `debian/source/format` | Source format, `3.0 (quilt)` |
| `debian/crc16.install` | Files moved from `debian/tmp` into the package |
| `debian/docs` | Extra documentation installed in `/usr/share/doc/crc16/` |

## How the packages are generated

`make deb` (see the `deb` target in `Makefile`) does the following:

1. Reads the version from `debian/changelog` and stops if its upstream part
   differs from `CRC16_VERSION` in `src/crc16.h`.
2. Deletes `build/deb/` and copies the source tree to
   `build/deb/crc16-<version>/`, leaving out `build/`, `.git` and `debian/`.
   The project tree is never touched.
3. Creates `build/deb/crc16_<version>.orig.tar.xz` from that copy. The
   `3.0 (quilt)` format requires this pristine upstream tarball. Entries are
   sorted and owned by root, and the timestamp comes from the changelog date,
   so the tarball is reproducible.
4. Copies `debian/` into the scratch copy.
5. Runs `dpkg-buildpackage` there (`-us -uc` by default). It calls
   `debian/rules`, which uses `dh`:
   - `dh_auto_build` runs the project `Makefile` with the flags from
     `dpkg-buildflags` (hardening, `-g`, `-ffile-prefix-map`) and
     `STD=c11`.
   - `dh_auto_test` runs `make check` (unit tests and CLI tests).
   - `dh_auto_install` runs `make install PREFIX=/usr` into `debian/tmp`.
   - `dh_install` moves `usr/bin` and `usr/share/man` into the package.
   - `dh_strip` splits the debug symbols into the `-dbgsym` package, and
     `dh_shlibdeps` computes the `Depends` field (`libc6`).
   - `dh_builddeb` writes the `.deb` files.

Everything ends up in `build/deb/`, which `make clean` removes.

## Output files

| File | What it is |
|------|------------|
| `crc16_<ver>_<arch>.deb` | The binary package: `/usr/bin/crc16`, the man page, `README.md`, copyright and changelog. This is what users install. |
| `crc16-dbgsym_<ver>_<arch>.deb` | Debug symbols stripped from the binary. Install only to debug or profile (gdb, perf, valgrind, core dumps). Skip with `DEB_BUILD_OPTIONS=noautodbgsym`. |
| `crc16_<ver>.orig.tar.xz` | Upstream source, without `debian/`. |
| `crc16_<ver>-<rev>.debian.tar.xz` | The `debian/` directory (plus automatic patches, if any). |
| `crc16_<ver>-<rev>.dsc` | Source package description with checksums. |
| `crc16_<ver>-<rev>_<arch>.changes` | Upload manifest listing every file above. Input for `lintian` and `dput`. |
| `crc16_<ver>-<rev>_<arch>.buildinfo` | Record of the build environment, used for reproducibility checks. |

The three source files (`.orig.tar.xz`, `.debian.tar.xz`, `.dsc`) let anyone
rebuild the package with `dpkg-source -x crc16_*.dsc && dpkg-buildpackage`.

## Standards adopted

- Debian Policy `4.7.4` (`Standards-Version` in `debian/control`).
- Source format `3.0 (quilt)` with an explicit `debian/source/format`.
- `debhelper-compat (= 14)`, declared as a build dependency, with no
  `debian/compat` file.
- `dpkg-build-api (= 1)`, which makes `Rules-Requires-Root: no` the default;
  the package builds without root or `fakeroot`.
- `debian/rules` provides the required targets through `dh`, and it honours
  `DEB_BUILD_OPTIONS` (`nocheck`, `nostrip`, `noopt`, `parallel=N`).
- Machine-readable `debian/copyright` (format 1.0), GPL-2+, with the pointer
  to `/usr/share/common-licenses/GPL-2`.
- Full hardening through `DEB_BUILD_MAINT_OPTIONS = hardening=+all`
  (PIE, relro, bindnow, stack protector, `_FORTIFY_SOURCE`, CET).
- Reproducible builds: `-ffile-prefix-map`, sorted and timestamped orig
  tarball, `SOURCE_DATE_EPOCH` from the changelog. Two consecutive builds
  were checked to give identical `.deb` files.
- Man page in section 1, compressed by `dh_compress`; `README.md` installed
  under `/usr/share/doc/crc16/`.
- In compat 14 the relationship substitution variables (`${shlibs:Depends}`,
  `${misc:Depends}`) are applied by `dh_gencontrol`, so they are not written
  in `debian/control`.

Choices that differ from the plain `Makefile`:

- `debian/rules` exports `STD=c11` instead of the default `c23`, so the
  package builds with any compiler in the archive. C11 is the project's
  minimum standard and does not change performance.
- The Debian build does not set `-march`, so the binary runs on any x86-64
  CPU. PCLMULQDQ folding is selected at run time.

## Releasing a new version

1. Update `CRC16_VERSION` in `src/crc16.h` and the version in the `.TH` line
   of `man/crc16.1`.
2. Add an entry to the top of `debian/changelog`, for example with
   `dch -v 1.1.0-1 "Describe the change."` (package `devscripts`), or by
   hand. The trailer date must be RFC 2822 (`date -R`), with two spaces
   before it.
3. Run `make deb` and `lintian --pedantic build/deb/*.changes`.
4. Commit and tag.

Use a new Debian revision (`1.0.0-2`) when only `debian/` changes and the
upstream version stays the same. Never rewrite an old changelog entry.

## Lintian status

`lintian --pedantic` currently reports:

- `initial-upload-closes-no-bugs` (W): the package is not meant for the
  Debian archive, so there is no ITP bug to close.
- `upstream-metadata-file-is-missing` (P): `debian/upstream/metadata` is
  optional. Lintian's informational level also suggests a `debian/watch`
  file, which makes no sense because the project publishes no release
  tarballs.

Any other tag should be fixed before a release.

## Submitting to Debian

The current packaging is suitable for local and third-party repository use.
For the official archive, a Debian Developer must sponsor the upload, an ITP
bug must be filed and closed in the first changelog entry
(`Closes: #NNNNNN`), and the package name `crc16` must not clash with an
existing binary. No `/usr/bin/crc16` exists in Debian at the time of writing.
See the Developer's Reference and the New Maintainers' Guide below.

## References

- Debian Policy Manual: <https://www.debian.org/doc/debian-policy/>
- Debian Policy, source packages (changelog, control, rules, copyright):
  <https://www.debian.org/doc/debian-policy/ch-source.html>
- Debian Policy, control file fields:
  <https://www.debian.org/doc/debian-policy/ch-controlfields.html>
- Machine-readable copyright format 1.0:
  <https://www.debian.org/doc/packaging-manuals/copyright-format/1.0/>
- Debian New Maintainers' Guide:
  <https://www.debian.org/doc/manuals/maint-guide/>
- Debian Developer's Reference:
  <https://www.debian.org/doc/manuals/developers-reference/>
- debhelper(7): <https://manpages.debian.org/unstable/debhelper/debhelper.7.en.html>
- dh(1): <https://manpages.debian.org/unstable/debhelper/dh.1.en.html>
- dpkg-source(1) (source formats):
  <https://manpages.debian.org/unstable/dpkg-dev/dpkg-source.1.en.html>
- dpkg-build-api(7):
  <https://manpages.debian.org/unstable/dpkg-dev/dpkg-build-api.7.en.html>
- dpkg-buildflags(1) and hardening:
  <https://wiki.debian.org/Hardening>
- Reproducible builds: <https://reproducible-builds.org/>
- Debug symbol packages: <https://wiki.debian.org/AutomaticDebugPackages>
- Lintian: <https://lintian.debian.org/>
