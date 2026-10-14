/* tz.c - time zones with daylight saving, from a POSIX TZ string */
#include <ctype.h>
#include <string.h>

#include "tz.h"
#include "amitime.h"

void tz_fixed(ars_tz *z, long minutes_west)
{
    memset(z, 0, sizeof *z);
    z->std_off = z->dst_off = minutes_west * 60;
}

/* --- parsing --- */

static const char *parse_name(const char *p)
{
    const char *start;

    if (*p == '<') {
        start = ++p;
        while (*p && *p != '>')
            p++;
        return *p == '>' && p - start >= 3 ? p + 1 : NULL;
    }
    start = p;
    while (isalpha((unsigned char)*p))
        p++;
    return p - start >= 3 ? p : NULL;
}

static const char *parse_num(const char *p, long *out, long max)
{
    long n = 0;

    if (!isdigit((unsigned char)*p))
        return NULL;
    while (isdigit((unsigned char)*p)) {
        n = n * 10 + (*p++ - '0');
        if (n > max)
            return NULL;
    }
    *out = n;
    return p;
}

/* [+-]hh[:mm[:ss]] in seconds. */
static const char *parse_hms(const char *p, long *out, long max_hours)
{
    long h, m = 0, s = 0;
    int neg = 0;

    if (*p == '+' || *p == '-')
        neg = *p++ == '-';
    if (!(p = parse_num(p, &h, max_hours)))
        return NULL;
    if (*p == ':') {
        if (!(p = parse_num(p + 1, &m, 59)))
            return NULL;
        if (*p == ':' && !(p = parse_num(p + 1, &s, 59)))
            return NULL;
    }
    *out = (h * 3600 + m * 60 + s) * (neg ? -1 : 1);
    return p;
}

static const char *parse_rule(const char *p, ars_tzrule *r)
{
    long a, b, c;

    memset(r, 0, sizeof *r);
    if (*p == 'M') {
        if (!(p = parse_num(p + 1, &a, 12)) || a < 1 || *p != '.'
            || !(p = parse_num(p + 1, &b, 5)) || b < 1 || *p != '.'
            || !(p = parse_num(p + 1, &c, 6)))
            return NULL;
        r->kind = 'M';
        r->month = (int)a;
        r->week = (int)b;
        r->wday = (int)c;
    } else if (*p == 'J') {
        if (!(p = parse_num(p + 1, &a, 365)) || a < 1)
            return NULL;
        r->kind = 'J';
        r->day = (int)a;
    } else {
        if (!(p = parse_num(p, &a, 365)))
            return NULL;
        r->kind = 'D';
        r->day = (int)a;
    }
    r->time = 2 * 3600;
    if (*p == '/' && !(p = parse_hms(p + 1, &r->time, 167)))
        return NULL;
    return p;
}

int tz_parse(ars_tz *z, const char *s)
{
    ars_tz t;
    const char *p = s;

    memset(&t, 0, sizeof t);
    if (!(p = parse_name(p)) || !(p = parse_hms(p, &t.std_off, 24)))
        return -1;
    t.dst_off = t.std_off;
    if (*p) {
        if (!(p = parse_name(p)))
            return -1;
        t.dst_off = t.std_off - 3600;
        if (*p && *p != ',' && !(p = parse_hms(p, &t.dst_off, 24)))
            return -1;
        if (*p != ',' || !(p = parse_rule(p + 1, &t.start))
            || *p != ',' || !(p = parse_rule(p + 1, &t.end)) || *p)
            return -1;
        t.has_dst = 1;
    }
    *z = t;
    return 0;
}

int tz_parse_tzone(ars_tz *z, const char *s)
{
    long secs;
    const char *p = parse_hms(s, &secs, 24);

    if (!p || *p || secs % 60)
        return -1;
    tz_fixed(z, secs / 60);
    return 0;
}

/* --- rules --- */

static int is_leap(long y)
{
    return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
}

/* Days from 1970-01-01 to y-m-d (Gregorian), the inverse of secs_to_civil. */
static int64_t days_from_civil(long y, int m, int d)
{
    int64_t era, yoe, doy, doe;

    y -= m <= 2;
    era = (y >= 0 ? y : y - 399) / 400;
    yoe = y - era * 400;
    doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
    doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

/* Local seconds since 1970 at which a rule fires in year y. */
static int64_t rule_local(const ars_tzrule *r, long y)
{
    static const int mdays[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    int64_t day;
    int first, mday, len;

    switch (r->kind) {
    case 'J':
        day = days_from_civil(y, 1, 1) + r->day - 1 + (is_leap(y) && r->day >= 60);
        break;
    case 'D':
        day = days_from_civil(y, 1, 1) + r->day;
        break;
    default:
        day = days_from_civil(y, r->month, 1);
        first = (int)((day % 7 + 11) % 7);   /* 1970-01-01 was a Thursday (4) */
        len = mdays[r->month - 1] + (r->month == 2 && is_leap(y));
        mday = 1 + (r->wday - first + 7) % 7 + (r->week - 1) * 7;
        while (mday > len)
            mday -= 7;
        day += mday - 1;
        break;
    }
    return day * 86400 + r->time;
}

static int in_dst(const ars_tz *z, int64_t utc)
{
    ars_tm tm;
    int64_t start, end;

    secs_to_civil(utc - z->std_off, &tm);
    start = rule_local(&z->start, tm.year) + z->std_off;
    end = rule_local(&z->end, tm.year) + z->dst_off;
    if (start < end)
        return utc >= start && utc < end;
    return utc < end || utc >= start;     /* southern hemisphere */
}

long tz_offset_utc(const ars_tz *z, int64_t utc)
{
    if (z->has_dst && in_dst(z, utc))
        return z->dst_off / 60;
    return z->std_off / 60;
}

long tz_offset_local(const ars_tz *z, int64_t local)
{
    if (z->has_dst && in_dst(z, local + z->dst_off))
        return z->dst_off / 60;
    return z->std_off / 60;
}
