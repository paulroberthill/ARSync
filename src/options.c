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
    return NULL;
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

    if ((target = numeric_option(o, name, &max)) != NULL) {
        if (eq) {
            val = eq + 1;
        } else if (*i + 1 < argc) {
            val = argv[++*i];
        } else {
            snprintf(err, errlen, "option --%s needs a value", name);
            return -1;
        }
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
    else if (strcmp(name, "no-motd") == 0)
        o->no_motd = 1;
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
    return 0;
}
