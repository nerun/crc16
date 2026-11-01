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

/* output.c - result formatters. */
#include "output.h"

#include "crc16.h"

#include <inttypes.h>

static void put_hex(const output_state *o, uint16_t v)
{
    fprintf(o->fp, o->lowercase ? "%04x" : "%04X", (unsigned)v);
}

static void put_csv_field(FILE *fp, const char *s)
{
    const char *p;
    int quote = 0;
    for (p = s; *p != '\0'; p++)
        if (*p == ',' || *p == '"' || *p == '\n' || *p == '\r') quote = 1;
    if (!quote) {
        fputs(s, fp);
        return;
    }
    fputc('"', fp);
    for (p = s; *p != '\0'; p++) {
        if (*p == '"') fputc('"', fp);
        fputc(*p, fp);
    }
    fputc('"', fp);
}

static void put_json_string(FILE *fp, const char *s)
{
    const unsigned char *p;
    fputc('"', fp);
    for (p = (const unsigned char *)s; *p != '\0'; p++) {
        if (*p == '"' || *p == '\\') fprintf(fp, "\\%c", *p);
        else if (*p < 0x20) fprintf(fp, "\\u%04x", (unsigned)*p);
        else fputc(*p, fp);
    }
    fputc('"', fp);
}

void output_begin(output_state *o)
{
    o->nprinted = 0;
    if (o->format == FMT_CSV) fputs("file,bytes,algorithm,crc\n", o->fp);
    else if (o->format == FMT_TSV) fputs("file\tbytes\talgorithm\tcrc\n", o->fp);
    else if (o->format == FMT_JSON) fputs("[", o->fp);
}

void output_file(output_state *o, const char *file, uint64_t bytes,
                 const size_t *ids, const uint16_t *results, size_t n)
{
    size_t i;

    switch (o->format) {
    case FMT_TEXT:
        if (o->nprinted > 0) fputc('\n', o->fp);
        if (o->title) fprintf(o->fp, "==> %s <==\n", file);
        if (!o->quiet) fprintf(o->fp, "Processed %" PRIu64 " bytes.\n", bytes);
        for (i = 0; i < n; i++) {
            fprintf(o->fp, "%-25s: ", crc16_variants[ids[i]].name);
            put_hex(o, results[i]);
            fputc('\n', o->fp);
        }
        break;
    case FMT_CSV:
    case FMT_TSV:
        for (i = 0; i < n; i++) {
            const char *name = crc16_variants[ids[i]].name;
            if (o->format == FMT_CSV) {
                put_csv_field(o->fp, file);
                fprintf(o->fp, ",%" PRIu64 ",%s,", bytes, name);
            } else {
                fprintf(o->fp, "%s\t%" PRIu64 "\t%s\t", file, bytes, name);
            }
            put_hex(o, results[i]);
            fputc('\n', o->fp);
        }
        break;
    case FMT_JSON:
        fputs(o->nprinted > 0 ? ",\n  {" : "\n  {", o->fp);
        fputs("\"file\": ", o->fp);
        put_json_string(o->fp, file);
        fprintf(o->fp, ", \"bytes\": %" PRIu64 ", \"crc\": {", bytes);
        for (i = 0; i < n; i++) {
            if (i > 0) fputs(", ", o->fp);
            put_json_string(o->fp, crc16_variants[ids[i]].name);
            fputs(": \"", o->fp);
            put_hex(o, results[i]);
            fputc('"', o->fp);
        }
        fputs("}}", o->fp);
        break;
    }
    o->nprinted++;
}

void output_end(output_state *o)
{
    if (o->format == FMT_JSON) fputs(o->nprinted > 0 ? "\n]\n" : "]\n", o->fp);
}

void output_list(FILE *fp, int lowercase)
{
    const char *hex = lowercase ? "0x%04x" : "0x%04X";
    size_t i;
    const char *const *a;

    fprintf(fp, "%-25s %-6s %-6s %-5s %-6s %-6s %-6s %s\n", "NAME", "POLY", "INIT",
            "REFIN", "REFOUT", "XOROUT", "CHECK", "ALIASES");
    for (i = 0; i < crc16_variant_count; i++) {
        const crc16_variant *v = &crc16_variants[i];
        fprintf(fp, "%-25s ", v->name);
        fprintf(fp, hex, (unsigned)v->poly);
        fputc(' ', fp);
        fprintf(fp, hex, (unsigned)v->init);
        fprintf(fp, " %-5s %-6s ", v->refin ? "true" : "false", v->refout ? "true" : "false");
        fprintf(fp, hex, (unsigned)v->xorout);
        fputc(' ', fp);
        fprintf(fp, hex, (unsigned)v->check);
        fputc(' ', fp);
        for (a = v->aliases; a != NULL && *a != NULL; a++)
            fprintf(fp, "%s%s", a == v->aliases ? "" : ", ", *a);
        fputc('\n', fp);
    }
}
