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
