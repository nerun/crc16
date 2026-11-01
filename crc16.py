#!/usr/bin/env python3
# -*- coding: utf-8 -*-
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

"""Calcula 31 variantes CRC-16, sem dependências externas.

Uso: python3 crc16 <arquivo>
Parâmetros no modelo CRC RevEng (polinômio e init na forma normal):
https://reveng.sourceforge.io/crc-catalogue/16.htm

Aliases consolidados:
  ARC: IBM, LHA, DF-1 (cálculo CRC; enquadramento do protocolo à parte).
  IBM-3740: CCITT-FALSE, AUTOSAR.
  IBM-SDLC: X-25, ISO-HDLC, ISO-IEC-14443-3-B.
  KERMIT: V-41-LSB, BLUETOOTH (init zero).
  MAXIM-DOW: MAXIM.
  SPI-FUJITSU: AUG-CCITT.
  UMTS: BUYPASS, VERIFONE.
  XMODEM: ACORN, LTE.
  GENIBUS: EPC, EPC-C1G2, I-CODE.
  PROFIBUS: IEC-61158-2.

Fletcher não é CRC; OpenPGP e CAN-FD não são CRC-16. SICK requer
outro algoritmo. EFI, G-3, EVEREST e ADCCP foram omitidos por falta
de uma especificação validada para as entradas originais.
"""

import argparse
import binascii
import sys
from functools import lru_cache

MASK = 0xFFFF
CHUNK_SIZE = 1024 * 1024
REVERSE_BYTES = bytes(int(format(i, "08b")[::-1], 2) for i in range(256))

# nome, poly, init, refin, refout, xorout
VARIANTS = (
    ("CRC-16/ARC", 0x8005, 0x0000, True, True, 0x0000),
    ("CRC-16/CDMA2000", 0xC867, 0xFFFF, False, False, 0x0000),
    ("CRC-16/CMS", 0x8005, 0xFFFF, False, False, 0x0000),
    ("CRC-16/DDS-110", 0x8005, 0x800D, False, False, 0x0000),
    ("CRC-16/DECT-R", 0x0589, 0x0000, False, False, 0x0001),
    ("CRC-16/DECT-X", 0x0589, 0x0000, False, False, 0x0000),
    ("CRC-16/DNP", 0x3D65, 0x0000, True, True, 0xFFFF),
    ("CRC-16/EN-13757", 0x3D65, 0x0000, False, False, 0xFFFF),
    ("CRC-16/GENIBUS", 0x1021, 0xFFFF, False, False, 0xFFFF),
    ("CRC-16/GSM", 0x1021, 0x0000, False, False, 0xFFFF),
    ("CRC-16/IBM-3740", 0x1021, 0xFFFF, False, False, 0x0000),
    ("CRC-16/IBM-SDLC", 0x1021, 0xFFFF, True, True, 0xFFFF),
    ("CRC-16/ISO-IEC-14443-3-A", 0x1021, 0xC6C6, True, True, 0x0000),
    ("CRC-16/KERMIT", 0x1021, 0x0000, True, True, 0x0000),
    ("CRC-16/LJ1200", 0x6F63, 0x0000, False, False, 0x0000),
    ("CRC-16/M17", 0x5935, 0xFFFF, False, False, 0x0000),
    ("CRC-16/MAXIM-DOW", 0x8005, 0x0000, True, True, 0xFFFF),
    ("CRC-16/MCRF4XX", 0x1021, 0xFFFF, True, True, 0x0000),
    ("CRC-16/MODBUS", 0x8005, 0xFFFF, True, True, 0x0000),
    ("CRC-16/NRSC-5", 0x080B, 0xFFFF, True, True, 0x0000),
    ("CRC-16/OPENSAFETY-A", 0x5935, 0x0000, False, False, 0x0000),
    ("CRC-16/OPENSAFETY-B", 0x755B, 0x0000, False, False, 0x0000),
    ("CRC-16/PROFIBUS", 0x1DCF, 0xFFFF, False, False, 0xFFFF),
    ("CRC-16/RIELLO", 0x1021, 0xB2AA, True, True, 0x0000),
    ("CRC-16/SPI-FUJITSU", 0x1021, 0x1D0F, False, False, 0x0000),
    ("CRC-16/T10-DIF", 0x8BB7, 0x0000, False, False, 0x0000),
    ("CRC-16/TELEDISK", 0xA097, 0x0000, False, False, 0x0000),
    ("CRC-16/TMS37157", 0x1021, 0x89EC, True, True, 0x0000),
    ("CRC-16/UMTS", 0x8005, 0x0000, False, False, 0x0000),
    ("CRC-16/USB", 0x8005, 0xFFFF, True, True, 0xFFFF),
    ("CRC-16/XMODEM", 0x1021, 0x0000, False, False, 0x0000),
)


def reflect16(value):
    return (REVERSE_BYTES[value & 0xFF] << 8) | REVERSE_BYTES[value >> 8]


@lru_cache(maxsize=64)
def make_table(poly):
    """Tabela MSB-first compartilhada pelas variantes do mesmo polinômio."""
    table = []
    for value in range(256):
        crc = value << 8
        for _ in range(8):
            crc = ((crc << 1) ^ (poly if crc & 0x8000 else 0)) & MASK
        table.append(crc)
    return tuple(table)


def update_crc(crc, data, poly):
    # Estado sempre na orientação normal; bytes já refletidos se necessário.
    if poly == 0x1021:
        return binascii.crc_hqx(data, crc)
    table = make_table(poly)
    for byte in data:
        crc = ((crc << 8) & MASK) ^ table[(crc >> 8) ^ byte]
    return crc


def finish_crc(crc, refout, xorout):
    if refout:
        crc = reflect16(crc)
    return crc ^ xorout


def crc16_optimized(data, poly, init=0x0000, refin=False,
                    refout=False, xorout=0x0000):
    """CRC de bytes; mesma interface original, com parâmetros RevEng.

    Mantém o registrador MSB-first: reflete os bytes de entrada quando
    refin=True e o resultado quando refout=True. Não mistura orientações.
    """
    if not 0 < poly <= MASK:
        raise ValueError("poly deve estar entre 0x0001 e 0xFFFF")
    if not 0 <= init <= MASK or not 0 <= xorout <= MASK:
        raise ValueError("init e xorout devem estar entre 0x0000 e 0xFFFF")
    data = bytes(data)
    if refin:
        data = data.translate(REVERSE_BYTES)
    return finish_crc(update_crc(init, data, poly), refout, xorout)


def calculate_file(stream):
    """Uma leitura do arquivo; memória limitada ao tamanho dos blocos."""
    states = [variant[2] for variant in VARIANTS]
    total = 0
    while True:
        block = stream.read(CHUNK_SIZE)
        if not block:
            break
        total += len(block)
        reflected = block.translate(REVERSE_BYTES)
        for i, (_, poly, _, refin, _, _) in enumerate(VARIANTS):
            states[i] = update_crc(states[i], reflected if refin else block, poly)
    results = [
        (name, finish_crc(states[i], refout, xorout))
        for i, (name, _, _, _, refout, xorout) in enumerate(VARIANTS)
    ]
    return total, results


def main():
    parser = argparse.ArgumentParser(
        description="Calcula 31 variantes CRC-16 de um arquivo.")
    parser.add_argument("arquivo", help="arquivo a processar em modo binário")
    args = parser.parse_args()
    try:
        with open(args.arquivo, "rb") as stream:
            total, results = calculate_file(stream)
        print(f"Processados {total} bytes.")
        for name, crc in results:
            print(f"{name:<25}: {crc:04X}")
    except OSError as error:
        print(f"Erro: {error}", file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        print("\nInterrompido.", file=sys.stderr)
        return 130
    return 0


if __name__ == "__main__":
    sys.exit(main())
