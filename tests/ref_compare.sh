#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# crc16 - compute many CRC-16 variants of a file in a single pass
# Copyright (C) 2026 Daniel Dias Rodrigues <danieldiasr@gmail.com>
#
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation; either version 2 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License along
# with this program; if not, see <https://www.gnu.org/licenses/>.

# Compare build/crc16 with the Python reference and exercise CLI error paths.
# Usage: sh tests/ref_compare.sh [path/to/crc16]
BIN=${1:-build/crc16}
case $BIN in /*) ;; *) BIN=$(pwd)/$BIN ;; esac # some checks change directory
ROOT=$(cd "$(dirname "$0")/.." && pwd)
TMP=$(mktemp -d) || exit 1
trap 'rm -rf "$TMP"' EXIT
fail=0

bad() { echo "FAIL: $*" >&2; fail=$((fail + 1)); }

# expect_status DESCRIPTION EXPECTED_STATUS COMMAND...
expect_status() {
    desc=$1; want=$2; shift 2
    "$@" >"$TMP/out" 2>"$TMP/err" </dev/null
    got=$?
    [ "$got" -eq "$want" ] || bad "$desc: exit $got, expected $want"
}

if command -v python3 >/dev/null 2>&1; then
    for n in 0 1 7 1048575 1048576 1048577 20000003; do
        head -c "$n" /dev/urandom >"$TMP/f$n"
        python3 "$ROOT/crc16.py" "$TMP/f$n" | sed '1s/^Processados/Processed/' >"$TMP/expected"
        for j in 1 2 5 0; do
            "$BIN" -j "$j" "$TMP/f$n" >"$TMP/actual" || bad "run on $n bytes (-j $j)"
            cmp -s "$TMP/expected" "$TMP/actual" || bad "output differs from crc16.py for $n bytes (-j $j)"
        done
    done
    echo "reference comparison done"
else
    echo "python3 not found: reference comparison skipped"
fi

# Buffer-boundary sizes: threaded and piped runs must equal the serial run.
for n in 4194303 4194304 4194305 8388608 9000001; do
    head -c "$n" /dev/urandom >"$TMP/b$n"
    "$BIN" -j 1 --no-mmap "$TMP/b$n" >"$TMP/serial" # plain fread, one thread
    for j in 1 2 7 0; do
        "$BIN" -j "$j" "$TMP/b$n" >"$TMP/threaded"
        cmp -s "$TMP/serial" "$TMP/threaded" || bad "mmap -j $j != fread serial for $n bytes"
        "$BIN" -j "$j" --no-mmap "$TMP/b$n" >"$TMP/threaded"
        cmp -s "$TMP/serial" "$TMP/threaded" || bad "fread -j $j != fread serial for $n bytes"
    done
    cat "$TMP/b$n" | "$BIN" -j 0 - | sed 's/^==> .*//' >"$TMP/piped"
    cmp -s "$TMP/serial" "$TMP/piped" || bad "piped != serial for $n bytes"
    rm -f "$TMP/b$n"
done

# Functional checks (no Python needed).
printf '123456789' >"$TMP/check"
"$BIN" -q -a arc "$TMP/check" >"$TMP/o"; [ "$(cat "$TMP/o")" = "$(printf '%-25s: %s' CRC-16/ARC BB3D)" ] || bad "-a arc"
"$BIN" -q -x -a ibm-sdlc,x-25 "$TMP/check" >"$TMP/o" 2>&1; [ "$(cat "$TMP/o")" = "$(printf '%-25s: %s' CRC-16/IBM-SDLC 906e)" ] || bad "alias + lowercase + dedupe"
"$BIN" -q -a CRC-16/xmodem - <"$TMP/check" >"$TMP/o"; [ "$(cat "$TMP/o")" = "$(printf '%-25s: %s' CRC-16/XMODEM 31C3)" ] || bad "stdin"
"$BIN" -q -a arc <"$TMP/check" >"$TMP/o"; [ "$(cat "$TMP/o")" = "$(printf '%-25s: %s' CRC-16/ARC BB3D)" ] || bad "implicit stdin"
"$BIN" -f csv -a arc "$TMP/check" | tail -n 1 | grep -q ',9,CRC-16/ARC,BB3D$' || bad "csv"
"$BIN" -f json -a arc "$TMP/check" | grep -q '"CRC-16/ARC": "BB3D"' || bad "json"
"$BIN" -f tsv -a arc "$TMP/check" | tail -n 1 | grep -q "$(printf '\t9\tCRC-16/ARC\tBB3D')\$" || bad "tsv"
"$BIN" -a arc "$TMP/check" "$TMP/check" | grep -c '^==> ' | grep -qx 2 || bad "multiple files"
# Tables are reused across files: each block must equal a run on that file alone.
printf 'abc' >"$TMP/abc"
"$BIN" "$TMP/abc" >"$TMP/o1"; "$BIN" "$TMP/check" >"$TMP/o2"
"$BIN" "$TMP/abc" "$TMP/check" >"$TMP/o"
{ echo "==> $TMP/abc <=="; cat "$TMP/o1"; echo; echo "==> $TMP/check <=="; cat "$TMP/o2"; } | cmp -s - "$TMP/o" || bad "context reuse across files"
"$BIN" -q -a arc - - <"$TMP/check" >"$TMP/o" || bad "stdin twice"
grep -c 'BB3D$' "$TMP/o" | grep -qx 1 || bad "stdin twice: second read must be empty"
cp "$TMP/check" "$TMP/-a"
(cd "$TMP" && "$BIN" -q -a arc -- -a) >"$TMP/o" || bad "--"
grep -q 'BB3D$' "$TMP/o" || bad "-- ends options"
"$BIN" -q -j 1 --self-test >/dev/null || bad "--self-test -j 1"
"$BIN" --self-test >/dev/null || bad "--self-test"
"$BIN" --list | grep -q 'CRC-16/ARC' || bad "--list"
"$BIN" --version | grep -q 'Minimum ISO C' || bad "--version"
"$BIN" --help | grep -q '^Usage:' || bad "--help"

# Error paths.
expect_status "missing file" 1 "$BIN" "$TMP/does-not-exist"
grep -q "^crc16: $TMP/does-not-exist: No such file or directory$" "$TMP/err" || bad "missing file message"
expect_status "directory" 1 "$BIN" "$TMP"
grep -q "^crc16: $TMP: Is a directory$" "$TMP/err" || bad "directory message"
: >"$TMP/secret"; chmod 000 "$TMP/secret"
if [ ! -r "$TMP/secret" ]; then
    expect_status "unreadable file" 1 "$BIN" "$TMP/secret"
    grep -q "Permission denied" "$TMP/err" || bad "unreadable message"
fi
expect_status "continue after failure" 1 "$BIN" "$TMP/does-not-exist" "$TMP/check"
grep -q 'Processed 9 bytes' "$TMP/out" || bad "no output for good file after a failure"
expect_status "bad jobs" 2 "$BIN" -j x "$TMP/check"
expect_status "negative jobs" 2 "$BIN" --jobs=-1 "$TMP/check"
expect_status "too many jobs" 2 "$BIN" -j 1025 "$TMP/check"
expect_status "flag with value" 2 "$BIN" --quiet=1 "$TMP/check"
expect_status "bad option" 2 "$BIN" --bogus
expect_status "bad short option" 2 "$BIN" -Z
expect_status "missing argument" 2 "$BIN" -a
expect_status "unknown algorithm" 2 "$BIN" -a nosuch "$TMP/check"
grep -q "unknown algorithm 'nosuch'" "$TMP/err" || bad "unknown algorithm message"
expect_status "bad format" 2 "$BIN" -f xml "$TMP/check"
expect_status "empty algorithm" 2 "$BIN" -a arc, "$TMP/check"

if [ "$fail" -ne 0 ]; then echo "$fail failure(s)" >&2; exit 1; fi
echo "ref_compare: all OK"
