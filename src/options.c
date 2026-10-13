/* options.c - command-line options
 *
 * Spelled like rsync's: short options can be bundled ("-vv"), long options
 * take values as "--opt=VAL" or "--opt VAL", and "--" ends option parsing.
 */
#include <stdio.h>
#include <string.h>

#include "options.h"

static int parse_uint(const char *s, int max, int *out)
{
    long v = 0;

    if (!*s)
        return -1;
    for (; *s; s++) {
        if (*s < '0' || *s > '9' || (v = v * 10 + (*s - '0')) > max)
            return -1;
    }
    *out = (int)v;
    return 0;
}

/* Long options that take a numeric value. */
static int *numeric_option(ars_options *o, const char *name, int *max)
{
    if (strcmp(name, "port") == 0) {
        *max = 65535;
        return &o->port;
    }
    if (strcmp(name, "timeout") == 0) {
        *max = 1000000;
        return &o->timeout;
    }
    if (strcmp(name, "max-name") == 0) {
        *max = 1024;
        return &o->max_name;
    }
    if (strcmp(name, "compress-level") == 0) {
        *max = 9;
        return &o->compress_level;
    }
    return NULL;
}

/* Long options that take a string value. Returns 0 if handled, -1 on an
 * error, 1 if name isn't one of them. */
static int string_option(ars_options *o, const char *name, const char *val,
                         char *err, size_t errlen)
{
    if (strcmp(name, "password-file") == 0) {
        o->password_file = val;
        return 0;
    }
    if (strcmp(name, "exclude") == 0)
        return filters_add(&o->filters, val, FILTER_ADD_EXCLUDE, err, errlen);
    if (strcmp(name, "include") == 0)
        return filters_add(&o->filters, val, FILTER_ADD_INCLUDE, err, errlen);
    if (strcmp(name, "filter") == 0)
        return filters_add(&o->filters, val, FILTER_ADD_RULE, err, errlen);
    if (strcmp(name, "exclude-from") == 0)
        return filters_add_file(&o->filters, val, FILTER_ADD_EXCLUDE, err, errlen);
    if (strcmp(name, "include-from") == 0)
        return filters_add_file(&o->filters, val, FILTER_ADD_INCLUDE, err, errlen);
    if (strcmp(name, "compress-choice") == 0 || strcmp(name, "zc") == 0) {
        if (strcmp(val, "zlib") == 0)
            o->compress_choice = CPRES_ZLIB;
        else if (strcmp(val, "zlibx") == 0)
            o->compress_choice = CPRES_ZLIBX;
        else {
            snprintf(err, errlen, "unsupported compress choice: %s (arsync has zlib and zlibx)", val);
            return -1;
        }
        o->compress = 1;
        return 0;
    }
    return 1;
}

static int takes_string(const char *name)
{
    static const char *const names[] = {
        "password-file", "exclude", "include", "filter", "exclude-from",
        "include-from", "compress-choice", "zc",
    };
    size_t i;

    for (i = 0; i < sizeof names / sizeof names[0]; i++)
        if (strcmp(name, names[i]) == 0)
            return 1;
    return 0;
}

static int parse_long(ars_options *o, const char *opt, int argc, char **argv, int *i,
                      char *err, size_t errlen)
{
    char name[32];
    const char *eq = strchr(opt, '='), *val;
    size_t len = eq ? (size_t)(eq - opt) : strlen(opt);
    int *target, max;

    if (len >= sizeof name) {
        snprintf(err, errlen, "unknown option --%s", opt);
        return -1;
    }
    memcpy(name, opt, len);
    name[len] = '\0';

    target = numeric_option(o, name, &max);
    if (target || takes_string(name)) {
        if (eq) {
            val = eq + 1;
        } else if (*i + 1 < argc) {
            val = argv[++*i];
        } else {
            snprintf(err, errlen, "option --%s needs a value", name);
            return -1;
        }
        if (!target)
            return string_option(o, name, val, err, errlen) < 0 ? -1 : 0;
        if (parse_uint(val, max, target) < 0) {
            snprintf(err, errlen, "invalid value for --%s: %s", name, val);
            return -1;
        }
        return 0;
    }

    if (eq) {
        snprintf(err, errlen, "option --%s takes no value", name);
        return -1;
    }
    if (strcmp(name, "verbose") == 0)
        o->verbose++;
    else if (strcmp(name, "recursive") == 0)
        o->recursive = 1;
    else if (strcmp(name, "list-only") == 0)
        o->list_only = 2;
    else if (strcmp(name, "times") == 0)
        o->times = 1;
    else if (strcmp(name, "dry-run") == 0)
        o->dry_run = 1;
    else if (strcmp(name, "whole-file") == 0)
        o->whole_file = 1;
    else if (strcmp(name, "progress") == 0)
        o->progress = 1;
    else if (strcmp(name, "amiga-rules") == 0)
        o->amiga_rules = 1;
    else if (strcmp(name, "archive") == 0)
        o->recursive = o->times = 1;
    else if (strcmp(name, "no-motd") == 0)
        o->no_motd = 1;
    else if (strcmp(name, "delete") == 0)
        o->delete_mode = 1;
    else if (strcmp(name, "delete-excluded") == 0)
        o->delete_mode = o->delete_excluded = 1;
    else if (strcmp(name, "compress") == 0)
        o->compress = 1;
    else if (strcmp(name, "help") == 0)
        o->show_help = 1;
    else if (strcmp(name, "version") == 0)
        o->show_version = 1;
    else {
        snprintf(err, errlen, "unknown option --%s", name);
        return -1;
    }
    return 0;
}

static int parse_short(ars_options *o, const char *opts, char *err, size_t errlen)
{
    for (; *opts; opts++) {
        switch (*opts) {
        case 'v':
            o->verbose++;
            break;
        case 'r':
            o->recursive = 1;
            break;
        case 't':
            o->times = 1;
            break;
        case 'n':
            o->dry_run = 1;
            break;
        case 'W':
            o->whole_file = 1;
            break;
        case 'P':
            /* rsync's -P is --partial --progress; arsync never keeps
             * partial files, so it's just --progress. */
            o->progress = 1;
            break;
        case 'a':
            /* rsync's -a is -rlptgoD; of those arsync supports -r and -t. */
            o->recursive = o->times = 1;
            break;
        case 'z':
            o->compress = 1;
            break;
        case 'h':
            o->show_help = 1;
            break;
        default:
            snprintf(err, errlen, "unknown option -%c", *opts);
            return -1;
        }
    }
    return 0;
}

int options_parse(ars_options *o, int argc, char **argv, char *err, size_t errlen)
{
    int i, only_args = 0;
    const char *a;

    memset(o, 0, sizeof *o);
    filters_init(&o->filters);
    o->max_name = -1;
    o->compress_level = -1;
    for (i = 1; i < argc; i++) {
        a = argv[i];
        if (!only_args && a[0] == '-' && a[1] != '\0') {
            if (strcmp(a, "--") == 0) {
                only_args = 1;
                continue;
            }
            if (a[1] == '-') {
                if (parse_long(o, a + 2, argc, argv, &i, err, errlen) < 0)
                    return -1;
            } else if (parse_short(o, a + 1, err, errlen) < 0) {
                return -1;
            }
            continue;
        }
        if (o->nargs == ARS_MAX_ARGS) {
            snprintf(err, errlen, "too many arguments: %s", a);
            return -1;
        }
        o->args[o->nargs++] = a;
    }
    if (o->delete_mode && !o->recursive) {
        snprintf(err, errlen, "--delete does not work without --recursive (-r)");
        return -1;
    }
    return 0;
}

void options_free(ars_options *o)
{
    filters_free(&o->filters);
}
