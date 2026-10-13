/* output.c - rsync-compatible formatting of numbers, modes, times, listings */
#include <stdio.h>
#include <string.h>

#include "output.h"
#include "amitime.h"
#include "plat.h"

const char *comma_num(int64_t n, char *buf)
{
    char digits[24];
    int len = 0, i, out = 0, neg = n < 0;
    uint64_t v = neg ? (uint64_t)0 - (uint64_t)n : (uint64_t)n;

    do {
        digits[len++] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    if (neg)
        buf[out++] = '-';
    for (i = len - 1; i >= 0; i--) {
        buf[out++] = digits[i];
        if (i > 0 && i % 3 == 0)
            buf[out++] = ',';
    }
    buf[out] = '\0';
    return buf;
}

const char *comma_num_frac2(int64_t hundredths, char *buf)
{
    size_t len;

    comma_num(hundredths / 100, buf);
    len = strlen(buf);
    snprintf(buf + len, 8, ".%02u", (unsigned)((hundredths < 0 ? -hundredths : hundredths) % 100));
    return buf;
}

const char *permstring(uint32_t mode, char *buf)
{
    static const char rwx[] = "rwxrwxrwx";
    int i;

    for (i = 0; i < 9; i++)
        buf[1 + i] = mode & (1u << (8 - i)) ? rwx[i] : '-';
    if (mode & WIRE_S_ISUID)
        buf[3] = mode & 0100 ? 's' : 'S';
    if (mode & WIRE_S_ISGID)
        buf[6] = mode & 0010 ? 's' : 'S';
    if (mode & WIRE_S_ISVTX)
        buf[9] = mode & 0001 ? 't' : 'T';

    switch (mode & WIRE_S_IFMT) {
    case WIRE_S_IFDIR: buf[0] = 'd'; break;
    case WIRE_S_IFLNK: buf[0] = 'l'; break;
    case WIRE_S_IFBLK: buf[0] = 'b'; break;
    case WIRE_S_IFCHR: buf[0] = 'c'; break;
    case WIRE_S_IFSOCK: buf[0] = 's'; break;
    case WIRE_S_IFIFO: buf[0] = 'p'; break;
    default: buf[0] = '-'; break;
    }
    buf[10] = '\0';
    return buf;
}

const char *timestring(int64_t t, char *buf)
{
    ars_tm tm;

    plat_localtime(t, &tm);
    snprintf(buf, 20, "%4d/%02d/%02d %02d:%02d:%02d",
             tm.year, tm.month, tm.day, tm.hour, tm.minute, tm.second);
    return buf;
}

void format_list_entry(const ars_flist *fl, const ars_file *f, char *buf, size_t bufsize)
{
    char perms[11], size[32], when[20];

    snprintf(buf, bufsize, "%s %14s %s %s", permstring(f->mode, perms),
             comma_num(f->size, size), timestring(f->mtime, when), flist_name(fl, f));
}

/* --- statistics --- */

void stats_count(long *counts, uint32_t mode)
{
    counts[STAT_ALL]++;
    switch (mode & WIRE_S_IFMT) {
    case WIRE_S_IFREG:
        break;
    case WIRE_S_IFDIR:
        counts[STAT_DIR]++;
        break;
    case WIRE_S_IFLNK:
        counts[STAT_LINK]++;
        break;
    case WIRE_S_IFCHR:
    case WIRE_S_IFBLK:
        counts[STAT_DEV]++;
        break;
    default:
        counts[STAT_SPECIAL]++;
    }
}

/* "Number of files: 12 (reg: 9, dir: 3)", as rsync's output_itemized_counts(). */
static void print_counts(const char *what, const long *counts)
{
    static const char *const labels[] = { "reg", "dir", "link", "dev", "special" };
    char num[32];
    const char *pre = " (";
    long v;
    int i;

    printf("%s: %s", what, comma_num(counts[STAT_ALL], num));
    if (counts[STAT_ALL]) {
        for (i = 0; i < 5; i++) {
            v = i ? counts[i]
                  : counts[STAT_ALL] - counts[STAT_DIR] - counts[STAT_LINK] - counts[STAT_DEV]
                    - counts[STAT_SPECIAL];
            if (v) {
                printf("%s%s: %s", pre, labels[i], comma_num(v, num));
                pre = ", ";
            }
        }
        printf(")");
    }
    printf("\n");
}

/* Milliseconds as rsync shows seconds: "1,234.567". */
static const char *comma_secs(int64_t ms, char *buf)
{
    size_t len;

    comma_num(ms / 1000, buf);
    len = strlen(buf);
    snprintf(buf + len, 8, ".%03u", (unsigned)(ms % 1000));
    return buf;
}

void print_summary(const ars_stats *st, int full, int protocol, long elapsed, int dry_run)
{
    char a[40], b[40], c[40];
    int64_t total = st->sent + st->received, speedup;

    if (full) {
        printf("\n");
        print_counts("Number of files", st->num_files);
        if (protocol >= 29)
            print_counts("Number of created files", st->created);
        if (protocol >= 31)
            print_counts("Number of deleted files", st->deleted);
        printf("Number of regular files transferred: %s\n", comma_num(st->xferred, a));
        printf("Total file size: %s bytes\n", comma_num(st->total_size, a));
        printf("Total transferred file size: %s bytes\n", comma_num(st->transferred_size, a));
        printf("Literal data: %s bytes\n", comma_num(st->literal, a));
        printf("Matched data: %s bytes\n", comma_num(st->matched, a));
        printf("File list size: %s\n", comma_num(st->flist_size, a));
        if (st->flist_buildtime) {
            printf("File list generation time: %s seconds\n", comma_secs(st->flist_buildtime, a));
            printf("File list transfer time: %s seconds\n", comma_secs(st->flist_xfertime, a));
        }
        printf("Total bytes sent: %s\n", comma_num(st->sent, a));
        printf("Total bytes received: %s\n", comma_num(st->received, a));
    }

    /* rsync's rate is bytes / (0.5 + whole seconds), shown with 2 decimals:
     * in hundredths that's bytes * 200 / (1 + 2 * seconds), rounded. */
    comma_num_frac2((total * 200 + elapsed) / (1 + 2 * (int64_t)elapsed), c);
    printf("\nsent %s bytes  received %s bytes  %s bytes/sec\n",
           comma_num(st->sent, a), comma_num(st->received, b), c);
    speedup = total ? (st->total_size * 100 + total / 2) / total : 0;
    printf("total size is %s  speedup is %s%s\n", comma_num(st->total_size, a),
           comma_num_frac2(speedup, b), dry_run ? " (DRY RUN)" : "");
}
