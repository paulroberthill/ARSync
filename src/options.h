/* options.h - command-line options */
#ifndef OPTIONS_H
#define OPTIONS_H

#include "arsync.h"
#include "filter.h"

#define ARS_MAX_ARGS 2

typedef struct ars_options {
    int verbose;
    int recursive;
    int times;          /* -t: preserve modification times */
    int dry_run;
    int whole_file;     /* -W: no delta transfer */
    int progress;
    int stats;          /* --stats */
    int itemize;        /* -i: 1, or 2 for -ii (unchanged items too) */
    int update;         /* -u: skip files that are newer on the receiver */
    int ignore_existing;/* --ignore-existing: only create new files */
    int existing;       /* --existing: only update files already there */
    int size_only;      /* --size-only: same size = up to date */
    int checksum;       /* -c: compare whole-file checksums, not times */
    int partial;        /* --partial: keep a partly received file */
    int max_name;       /* --max-name, -1 = detect */
    int amiga_rules;    /* apply AmigaDOS limits on any system (testing) */
    int list_only;      /* 1 = implied (no destination), 2 = --list-only */
    int port;           /* --port, 0 = default */
    int timeout;        /* --timeout seconds, 0 = none */
    int no_motd;
    int delete_mode;    /* --delete (or --delete-excluded) */
    int delete_excluded;
    int compress;       /* -z */
    int compress_level; /* --compress-level, -1 = default */
    int compress_choice;/* --compress-choice: 0 = negotiate, else CPRES_* */
    ars_filters filters;
    const char *password_file;
    int show_help;
    int show_version;
    int nargs;
    const char *args[ARS_MAX_ARGS];
} ars_options;

/* Compression methods (rsync's names: "zlib", "zlibx"). */
#define CPRES_NONE 0
#define CPRES_ZLIB 1        /* matched data also feeds the compressor */
#define CPRES_ZLIBX 2       /* only literal data is compressed */

/* Returns 0, or -1 with a message in err. */
int options_parse(ars_options *o, int argc, char **argv, char *err, size_t errlen);
void options_free(ars_options *o);

#endif
