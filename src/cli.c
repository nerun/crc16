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

/* cli.c - argument parsing. */
#include "cli.h"

#include <stdlib.h>
#include <string.h>

static void set_err(char *err, size_t size, const char *a, const char *b, const char *c)
{
    size_t n = 0;
    const char *parts[3];
    size_t k;
    parts[0] = a;
    parts[1] = b;
    parts[2] = c;
    for (k = 0; k < 3 && size > 0; k++) {
        const char *s = parts[k];
        while (s != NULL && *s != '\0' && n + 1 < size) err[n++] = *s++;
    }
    if (size > 0) err[n] = '\0';
}

static int ci_equal(const char *a, const char *b)
{
    for (; *a != '\0' && *b != '\0'; a++, b++) {
        char ca = *a, cb = *b;
        if (ca >= 'A' && ca <= 'Z') ca = (char)(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z') cb = (char)(cb - 'A' + 'a');
        if (ca != cb) return 0;
    }
    return *a == *b;
}

static int add_algorithms(cli_options *opt, const char *list, char *err, size_t errsize)
{
    char token[128];

    while (1) {
        size_t len = strcspn(list, ",");
        size_t id, k;

        if (len == 0 || len >= sizeof token) {
            set_err(err, errsize, len == 0 ? "empty algorithm name" : "algorithm name too long", NULL, NULL);
            return -1;
        }
        memcpy(token, list, len);
        token[len] = '\0';
        id = crc16_find_variant(token);
        if (id == CRC16_NOT_FOUND) {
            set_err(err, errsize, "unknown algorithm '", token, "' (see --list)");
            return -1;
        }
        for (k = 0; k < opt->nids; k++)
            if (opt->ids[k] == id) break;
        if (k == opt->nids) opt->ids[opt->nids++] = id;

        list += len;
        if (*list == '\0') return 0;
        list++; /* skip the comma; a trailing comma yields an empty name */
    }
}

static int set_format(cli_options *opt, const char *s, char *err, size_t errsize)
{
    if (ci_equal(s, "text")) opt->format = FMT_TEXT;
    else if (ci_equal(s, "csv")) opt->format = FMT_CSV;
    else if (ci_equal(s, "tsv")) opt->format = FMT_TSV;
    else if (ci_equal(s, "json")) opt->format = FMT_JSON;
    else {
        set_err(err, errsize, "unknown format '", s, "' (text, csv, tsv, json)");
        return -1;
    }
    return 0;
}

static int set_jobs(cli_options *opt, const char *s, char *err, size_t errsize)
{
    unsigned long n = 0;
    const char *p = s;

    if (*p == '\0') n = 1000000;
    for (; *p >= '0' && *p <= '9' && n <= 1000000; p++) n = n * 10 + (unsigned long)(*p - '0');
    if (*p != '\0' || n > 1024) {
        set_err(err, errsize, "invalid number of jobs '", s, "' (0 = auto, max 1024)");
        return -1;
    }
    opt->jobs = (unsigned)n;
    return 0;
}

static int apply_value(cli_options *opt, char key, const char *value, char *err, size_t errsize)
{
    if (key == 'a') return add_algorithms(opt, value, err, errsize);
    if (key == 'j') return set_jobs(opt, value, err, errsize);
    return set_format(opt, value, err, errsize);
}

int cli_parse(int argc, char **argv, cli_options *opt, char *err, size_t errsize)
{
    int i;
    int past_dd = 0;

    memset(opt, 0, sizeof *opt);
    opt->format = FMT_TEXT;
    opt->files = (const char **)calloc((size_t)argc + 1, sizeof *opt->files);
    if (opt->files == NULL) {
        set_err(err, errsize, "out of memory", NULL, NULL);
        return -1;
    }

    for (i = 1; i < argc; i++) {
        const char *arg = argv[i];

        if (past_dd || arg[0] != '-' || arg[1] == '\0') {
            opt->files[opt->nfiles++] = arg;
        } else if (strcmp(arg, "--") == 0) {
            past_dd = 1;
        } else if (arg[1] == '-') {
            const char *name = arg + 2;
            const char *eq = strchr(name, '=');
            size_t nlen = eq != NULL ? (size_t)(eq - name) : strlen(name);
            char key = 0;
            int flag = 0;

            if (nlen == 9 && strncmp(name, "algorithm", 9) == 0) key = 'a';
            else if (nlen == 6 && strncmp(name, "format", 6) == 0) key = 'f';
            else if (nlen == 4 && strncmp(name, "jobs", 4) == 0) key = 'j';
            else if (nlen == 4 && strncmp(name, "list", 4) == 0) flag = 'l';
            else if (nlen == 5 && strncmp(name, "quiet", 5) == 0) flag = 'q';
            else if (nlen == 9 && strncmp(name, "lowercase", 9) == 0) flag = 'x';
            else if (nlen == 9 && strncmp(name, "self-test", 9) == 0) flag = 's';
            else if (nlen == 7 && strncmp(name, "no-mmap", 7) == 0) flag = 'm';
            else if (nlen == 4 && strncmp(name, "help", 4) == 0) flag = 'h';
            else if (nlen == 7 && strncmp(name, "version", 7) == 0) flag = 'V';
            else {
                set_err(err, errsize, "unknown option '", arg, "'");
                return -1;
            }
            if (key != 0) {
                const char *value = eq != NULL ? eq + 1 : (i + 1 < argc ? argv[++i] : NULL);
                if (value == NULL) {
                    set_err(err, errsize, "option '", arg, "' requires an argument");
                    return -1;
                }
                if (apply_value(opt, key, value, err, errsize) != 0) return -1;
            } else {
                if (eq != NULL) {
                    set_err(err, errsize, "option '", arg, "' does not take an argument");
                    return -1;
                }
                switch (flag) {
                case 'l': opt->list = 1; break;
                case 'q': opt->quiet = 1; break;
                case 'x': opt->lowercase = 1; break;
                case 's': opt->self_test = 1; break;
                case 'm': opt->no_mmap = 1; break;
                case 'h': opt->help = 1; return 0;
                default: opt->version = 1; return 0;
                }
            }
        } else {
            const char *p;
            for (p = arg + 1; *p != '\0'; p++) {
                if (*p == 'a' || *p == 'f' || *p == 'j') {
                    const char *value = p[1] != '\0' ? p + 1 : (i + 1 < argc ? argv[++i] : NULL);
                    if (value == NULL) {
                        char opt_text[3];
                        opt_text[0] = '-';
                        opt_text[1] = *p;
                        opt_text[2] = '\0';
                        set_err(err, errsize, "option '", opt_text, "' requires an argument");
                        return -1;
                    }
                    if (apply_value(opt, *p, value, err, errsize) != 0) return -1;
                    break; /* the rest of the word was the value */
                }
                switch (*p) {
                case 'l': opt->list = 1; break;
                case 'q': opt->quiet = 1; break;
                case 'x': opt->lowercase = 1; break;
                case 'h': opt->help = 1; return 0;
                case 'V': opt->version = 1; return 0;
                default: {
                    char opt_text[3];
                    opt_text[0] = '-';
                    opt_text[1] = *p;
                    opt_text[2] = '\0';
                    set_err(err, errsize, "unknown option '", opt_text, "'");
                    return -1;
                }
                }
            }
        }
    }

    if (opt->nids == 0) {
        size_t k;
        for (k = 0; k < crc16_variant_count; k++) opt->ids[k] = k;
        opt->nids = crc16_variant_count;
    }
    return 0;
}

void cli_free(cli_options *opt)
{
    free((void *)opt->files);
    opt->files = NULL;
}

void cli_usage(FILE *fp)
{
    fputs("Usage: crc16 [OPTION]... [FILE]...\n"
          "Compute many CRC-16 variants of each FILE in a single pass.\n"
          "With no FILE, or when FILE is -, read standard input.\n"
          "\n"
          "  -a, --algorithm NAME  select variant(s); repeatable or comma list;\n"
          "                        case-insensitive; aliases and the CRC-16/\n"
          "                        prefix are accepted (default: all)\n"
          "  -l, --list            list variants with parameters, check values\n"
          "                        and aliases\n"
          "  -f, --format FMT      output format: text (default), csv, tsv, json\n"
          "  -j, --jobs N          worker threads (0 = one per CPU, the default;\n"
          "                        1 = single-threaded; at most 1024)\n"
          "  -x, --lowercase       print hexadecimal digits in lowercase\n"
          "  -q, --quiet           omit the \"Processed N bytes.\" line (text)\n"
          "      --no-mmap         read files with fread instead of mapping them\n"
          "      --self-test       verify every variant against its check value,\n"
          "                        and the fast paths against a bitwise reference\n"
          "  -h, --help            display this help and exit\n"
          "  -V, --version         output version information and exit\n"
          "\n"
          "Exit status: 0 success, 1 at least one file failed, 2 usage error.\n",
          fp);
}
