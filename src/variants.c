/* SPDX-License-Identifier: GPL-2.0-or-later
 * crc16 - compute many CRC-16 variants of a file in a single pass
 * Copyright (C) 2026 Daniel Dias Rodrigues <danieldiasr@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, see <https://www.gnu.org/licenses/>.
 */

/* variants.c - table of supported CRC-16 variants (mirrors crc16.py). */
#include "crc16.h"

#include <string.h>

static const char *const al_arc[] = {"IBM", "LHA", "DF-1", NULL};
static const char *const al_3740[] = {"CCITT-FALSE", "AUTOSAR", NULL};
static const char *const al_sdlc[] = {"X-25", "ISO-HDLC", "ISO-IEC-14443-3-B", NULL};
static const char *const al_kermit[] = {"V-41-LSB", "BLUETOOTH", NULL};
static const char *const al_maxim[] = {"MAXIM", NULL};
static const char *const al_spi[] = {"AUG-CCITT", NULL};
static const char *const al_umts[] = {"BUYPASS", "VERIFONE", NULL};
static const char *const al_xmodem[] = {"ACORN", "LTE", NULL};
static const char *const al_genibus[] = {"EPC", "EPC-C1G2", "I-CODE", NULL};
static const char *const al_profibus[] = {"IEC-61158-2", NULL};

/* name, aliases, poly, init, xorout, check, refin, refout */
const crc16_variant crc16_variants[] = {
    {"CRC-16/ARC", al_arc, 0x8005, 0x0000, 0x0000, 0xBB3D, 1, 1},
    {"CRC-16/CDMA2000", NULL, 0xC867, 0xFFFF, 0x0000, 0x4C06, 0, 0},
    {"CRC-16/CMS", NULL, 0x8005, 0xFFFF, 0x0000, 0xAEE7, 0, 0},
    {"CRC-16/DDS-110", NULL, 0x8005, 0x800D, 0x0000, 0x9ECF, 0, 0},
    {"CRC-16/DECT-R", NULL, 0x0589, 0x0000, 0x0001, 0x007E, 0, 0},
    {"CRC-16/DECT-X", NULL, 0x0589, 0x0000, 0x0000, 0x007F, 0, 0},
    {"CRC-16/DNP", NULL, 0x3D65, 0x0000, 0xFFFF, 0xEA82, 1, 1},
    {"CRC-16/EN-13757", NULL, 0x3D65, 0x0000, 0xFFFF, 0xC2B7, 0, 0},
    {"CRC-16/GENIBUS", al_genibus, 0x1021, 0xFFFF, 0xFFFF, 0xD64E, 0, 0},
    {"CRC-16/GSM", NULL, 0x1021, 0x0000, 0xFFFF, 0xCE3C, 0, 0},
    {"CRC-16/IBM-3740", al_3740, 0x1021, 0xFFFF, 0x0000, 0x29B1, 0, 0},
    {"CRC-16/IBM-SDLC", al_sdlc, 0x1021, 0xFFFF, 0xFFFF, 0x906E, 1, 1},
    {"CRC-16/ISO-IEC-14443-3-A", NULL, 0x1021, 0xC6C6, 0x0000, 0xBF05, 1, 1},
    {"CRC-16/KERMIT", al_kermit, 0x1021, 0x0000, 0x0000, 0x2189, 1, 1},
    {"CRC-16/LJ1200", NULL, 0x6F63, 0x0000, 0x0000, 0xBDF4, 0, 0},
    {"CRC-16/M17", NULL, 0x5935, 0xFFFF, 0x0000, 0x772B, 0, 0},
    {"CRC-16/MAXIM-DOW", al_maxim, 0x8005, 0x0000, 0xFFFF, 0x44C2, 1, 1},
    {"CRC-16/MCRF4XX", NULL, 0x1021, 0xFFFF, 0x0000, 0x6F91, 1, 1},
    {"CRC-16/MODBUS", NULL, 0x8005, 0xFFFF, 0x0000, 0x4B37, 1, 1},
    {"CRC-16/NRSC-5", NULL, 0x080B, 0xFFFF, 0x0000, 0xA066, 1, 1},
    {"CRC-16/OPENSAFETY-A", NULL, 0x5935, 0x0000, 0x0000, 0x5D38, 0, 0},
    {"CRC-16/OPENSAFETY-B", NULL, 0x755B, 0x0000, 0x0000, 0x20FE, 0, 0},
    {"CRC-16/PROFIBUS", al_profibus, 0x1DCF, 0xFFFF, 0xFFFF, 0xA819, 0, 0},
    {"CRC-16/RIELLO", NULL, 0x1021, 0xB2AA, 0x0000, 0x63D0, 1, 1},
    {"CRC-16/SPI-FUJITSU", al_spi, 0x1021, 0x1D0F, 0x0000, 0xE5CC, 0, 0},
    {"CRC-16/T10-DIF", NULL, 0x8BB7, 0x0000, 0x0000, 0xD0DB, 0, 0},
    {"CRC-16/TELEDISK", NULL, 0xA097, 0x0000, 0x0000, 0x0FB3, 0, 0},
    {"CRC-16/TMS37157", NULL, 0x1021, 0x89EC, 0x0000, 0x26B1, 1, 1},
    {"CRC-16/UMTS", al_umts, 0x8005, 0x0000, 0x0000, 0xFEE8, 0, 0},
    {"CRC-16/USB", NULL, 0x8005, 0xFFFF, 0xFFFF, 0xB4C8, 1, 1},
    {"CRC-16/XMODEM", al_xmodem, 0x1021, 0x0000, 0x0000, 0x31C3, 0, 0},
};

const size_t crc16_variant_count = sizeof crc16_variants / sizeof crc16_variants[0];

/* C99-compatible static assertion: the fixed-size arrays in crc16_ctx and
 * cli_options hold every variant (the array size is -1, an error, if not). */
typedef char crc16_assert_table_fits[sizeof crc16_variants / sizeof crc16_variants[0] <=
                                             CRC16_MAX_VARIANTS
                                         ? 1
                                         : -1];

static int ci_equal(const char *a, const char *b)
{
    for (; *a != '\0' && *b != '\0'; a++, b++) {
        int ca = (unsigned char)*a, cb = (unsigned char)*b;
        if (ca >= 'a' && ca <= 'z') ca -= 'a' - 'A';
        if (cb >= 'a' && cb <= 'z') cb -= 'a' - 'A';
        if (ca != cb) return 0;
    }
    return *a == *b;
}

size_t crc16_find_variant(const char *name)
{
    static const char prefix[] = "CRC-16/";
    const size_t plen = sizeof prefix - 1;
    size_t i;
    const char *const *a;

    /* Strip an optional case-insensitive "CRC-16/" prefix from the query. */
    {
        char head[sizeof prefix];
        size_t k;
        if (strlen(name) >= plen) {
            for (k = 0; k < plen; k++) head[k] = name[k];
            head[plen] = '\0';
            if (ci_equal(head, prefix)) name += plen;
        }
    }
    for (i = 0; i < crc16_variant_count; i++) {
        const crc16_variant *v = &crc16_variants[i];
        if (ci_equal(name, v->name + plen)) return i;
        for (a = v->aliases; a != NULL && *a != NULL; a++)
            if (ci_equal(name, *a)) return i;
    }
    return CRC16_NOT_FOUND;
}
