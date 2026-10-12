/* options.h - command-line options */
#ifndef OPTIONS_H
#define OPTIONS_H

#include "arsync.h"

#define ARS_MAX_ARGS 2

typedef struct ars_options {
    int verbose;
    int recursive;
    int list_only;      /* 1 = implied (no destination), 2 = --list-only */
    int port;           /* --port, 0 = default */
    int timeout;        /* --timeout seconds, 0 = none */
    int no_motd;
    const char *password_file;
    int show_help;
    int show_version;
    int nargs;
    const char *args[ARS_MAX_ARGS];
} ars_options;

/* Returns 0, or -1 with a message in err. */
int options_parse(ars_options *o, int argc, char **argv, char *err, size_t errlen);

#endif
