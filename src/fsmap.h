/* fsmap.h - which incoming files the local filesystem can hold
 *
 * Names and sizes that AmigaDOS can't store are skipped with a warning (and
 * exit code 23), never silently altered: a truncated or renamed file would
 * look like a different file to the next run.
 */
#ifndef FSMAP_H
#define FSMAP_H

#include "flist.h"

typedef struct ars_fsrules {
    int name_max;           /* longest name component; 0 = no limit */
    int case_insensitive;   /* "Foo" and "foo" are the same file */
    int64_t max_file_size;
} ars_fsrules;

/* Why a name can't be stored as is, or NULL if it can. Checks only the
 * last component: parent directories were checked as entries of their own. */
const char *fsmap_check_name(const ars_fsrules *r, const char *path);

/* Marks entries whose full path equals an earlier one's (by index) when case
 * is ignored, by setting mark in flags[i]. Returns how many were marked, or
 * -1 if out of memory. */
long fsmap_find_case_clashes(const ars_flist *fl, uint8_t *flags, uint8_t mark);

#endif
