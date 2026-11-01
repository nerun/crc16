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

/* output.h - result formatters (text, csv, tsv, json). */
#ifndef OUTPUT_H
#define OUTPUT_H

#include <stdint.h>
#include <stdio.h>

typedef enum output_format {
    FMT_TEXT,
    FMT_CSV,
    FMT_TSV,
    FMT_JSON
} output_format;

typedef struct output_state {
    FILE *fp;
    output_format format;
    int lowercase;
    int quiet;
    int title;     /* print a "==> FILE <==" header (text, several files) */
    int nprinted;  /* number of file blocks already written */
} output_state;

void output_begin(output_state *o);
/* results[i] belongs to variant ids[i]. */
void output_file(output_state *o, const char *file, uint64_t bytes,
                 const size_t *ids, const uint16_t *results, size_t n);
void output_end(output_state *o);

void output_list(FILE *fp, int lowercase);

#endif /* OUTPUT_H */
