/* output.h - rsync-compatible formatting of numbers, modes, times, listings */
#ifndef OUTPUT_H
#define OUTPUT_H

#include "flist.h"

/* 1234567 -> "1,234,567". buf needs 32 bytes. */
const char *comma_num(int64_t n, char *buf);
/* A value in hundredths, e.g. 19000 -> "190.00", with grouping. buf: 40 bytes. */
const char *comma_num_frac2(int64_t hundredths, char *buf);
/* "drwxr-xr-x" style, from a wire mode. buf needs 11 bytes. */
const char *permstring(uint32_t mode, char *buf);
/* "2026/10/12 14:00:00" in local time. buf needs 20 bytes. */
const char *timestring(int64_t t, char *buf);
/* One --list-only line (without newline), as rsync prints it. */
void format_list_entry(const ars_flist *fl, const ars_file *f, char *buf, size_t bufsize);

/* --- items (one line per file of -v / -i output) --- */

/* rsync's %i change summary, "YXcstpoguax": >f.st...... and so on.
 * sending picks '<' over '>' for transfers. buf needs 12 bytes. */
const char *itemize_string(long iflags, uint32_t mode, int sending, char *buf);
/* Whether rsync's receiver or sender would log a non-transfer item
 * (maybe_log_item()): any change with -i (everything with -ii, or -i -vv);
 * otherwise only directories that changed and local changes. */
int item_shown(int itemize, int verbose, long iflags, uint32_t mode);
/* Prints an item as "name" or, with -i, "%i name" ("dir/" for directories). */
void print_item(int itemize, const char *name, uint32_t mode, long iflags, int sending);
/* A deletion: "deleting name", or "*deleting   name" with -i. */
void print_deleted(int itemize, const char *name, int is_dir);

/* Transfer statistics, as rsync keeps them for -v and --stats. The
 * per-type counts are indexed by STAT_*: [STAT_ALL] is the total, the
 * others are the non-regular items within it. */
#define STAT_ALL 0
#define STAT_DIR 1
#define STAT_LINK 2
#define STAT_DEV 3
#define STAT_SPECIAL 4

typedef struct ars_stats {
    long num_files[5];
    long created[5];
    long deleted[5];
    long xferred;               /* regular files transferred */
    int64_t total_size;
    int64_t transferred_size;   /* total size of the files transferred */
    int64_t literal;            /* file data sent as data */
    int64_t matched;            /* file data covered by blocks of old files */
    int64_t flist_size;         /* bytes of the file list on the wire */
    int64_t flist_buildtime;    /* ms; 0 = unknown (no times shown) */
    int64_t flist_xfertime;     /* ms */
    int64_t sent, received;
} ars_stats;

/* Counts one item of the given wire mode in counts[]. */
void stats_count(long *counts, uint32_t mode);
/* The --stats block (when full) and the -v summary lines. */
void print_summary(const ars_stats *st, int full, int protocol, long elapsed, int dry_run);

#endif
