/* filter.h - include/exclude rules (--exclude, --include, --filter and the
 * -from file variants), matched the way rsync matches them */
#ifndef FILTER_H
#define FILTER_H

#include "wire.h"

/* Rule flags. */
#define FILTER_INCLUDE 1        /* "+": an include rule (else exclude) */
#define FILTER_DIR_ONLY 2       /* pattern ended in '/': directories only */
#define FILTER_WILD 4           /* has *, ? or [ */
#define FILTER_WILD2 8          /* has "**" */
#define FILTER_WILD2_PREFIX 16  /* starts with "**" */
#define FILTER_WILD3_SUFFIX 32  /* ends in "/" + "***": the dir and everything in it */

typedef struct ars_filter_rule {
    char *pattern;      /* without the trailing '/'; a leading '/' anchors */
    unsigned flags;
    int slash_cnt;      /* slashes in the pattern (a leading one too) */
} ars_filter_rule;

typedef struct ars_filters {
    ars_filter_rule *rules;
    size_t count, cap;
} ars_filters;

/* How filters_add() reads its text. */
#define FILTER_ADD_EXCLUDE 0    /* --exclude: a pattern, or "+ PAT" / "- PAT" */
#define FILTER_ADD_INCLUDE 1    /* --include: likewise, include by default */
#define FILTER_ADD_RULE 2       /* --filter: "- PAT", "+ PAT", "exclude PAT", ... */

void filters_init(ars_filters *f);
void filters_free(ars_filters *f);

/* Adds one rule (or, for "!", clears the list). Returns 0, or -1 with a
 * message in err. */
int filters_add(ars_filters *f, const char *text, int how, char *err, size_t errlen);

/* Adds a rule per line of a file. Empty lines and lines starting with ';'
 * or '#' are skipped. Returns 0, or -1 with a message in err. */
int filters_add_file(ars_filters *f, const char *path, int how, char *err, size_t errlen);

/* Whether name (a path relative to the top of the transfer, as in the
 * file list) is excluded: the first matching rule decides; with none it's
 * included. */
int filters_excluded(const ars_filters *f, const char *name, int is_dir);

/* Sends the rules as rsync does ("- PAT" / "+ PAT", one int32-prefixed
 * string each), ending with an int32 0. */
void filters_send(const ars_filters *f, ars_writer *w);

/* rsync's wildcard matching of one string: '*' stops at '/', "**" doesn't,
 * '?' is any character but '/', "[...]" a character class, '\' quotes.
 * Exported for tests. */
int wildmatch(const char *pattern, const char *text);

#endif
