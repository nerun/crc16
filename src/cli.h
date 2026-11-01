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

/* cli.h - hand-written command-line parser (no getopt_long: not ISO C). */
#ifndef CLI_H
#define CLI_H

#include "crc16.h"
#include "output.h"

#include <stdio.h>

typedef struct cli_options {
    size_t ids[CRC16_MAX_VARIANTS]; /* selected variants (all when none given) */
    size_t nids;
    output_format format;
    int lowercase;
    int quiet;
    int list;
    int self_test;
    int help;
    int version;
    int no_mmap; /* always read with fread */
    unsigned jobs; /* worker threads; 0 = auto */
    const char **files; /* points into argv; "-" means stdin */
    size_t nfiles;
} cli_options;

/* Returns 0 on success; otherwise -1 with a message in err. */
int cli_parse(int argc, char **argv, cli_options *opt, char *err, size_t errsize);
void cli_free(cli_options *opt);
void cli_usage(FILE *fp);

#endif /* CLI_H */
