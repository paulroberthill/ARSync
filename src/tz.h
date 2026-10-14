/* tz.h - time zones with daylight saving, from a POSIX TZ string
 *
 * AmigaOS 3.x keeps a single offset (Locale's loc_GMTOffset) and no
 * daylight-saving rule, while the clock itself is usually moved to summer
 * time by hand or by a tool. File stamps are therefore in whichever offset
 * was in force when they were written. A POSIX rule such as
 * "GMT0BST,M3.5.0/1,M10.5.0" (from ENV:TZ) lets each stamp be converted
 * with the offset for its own date.
 *
 * Offsets are minutes west of Greenwich, as in loc_GMTOffset and POSIX TZ:
 * local time = UTC - offset.
 */
#ifndef TZ_H
#define TZ_H

#include "arsync.h"

typedef struct ars_tzrule {
    char kind;          /* 'J' (1-365, no Feb 29), 'D' (0-365), 'M' (month.week.day) */
    int month, week, wday;
    int day;
    long time;          /* seconds after local midnight; may be negative or > 24h */
} ars_tzrule;

typedef struct ars_tz {
    long std_off, dst_off;  /* seconds west */
    int has_dst;
    ars_tzrule start, end;  /* DST starts at start (in standard time), ends at end (in DST) */
} ars_tz;

/* A zone with no daylight saving. */
void tz_fixed(ars_tz *z, long minutes_west);

/* Parses a POSIX TZ string (std offset [dst [offset] ,start[/time],end[/time]]).
 * A DST name without a rule is rejected rather than guessed. Returns 0, or
 * -1 if the string isn't valid (z is then unchanged). */
int tz_parse(ars_tz *z, const char *s);

/* Parses ENV:TZONE as set by SNTP: whole hours west of Greenwich, optionally
 * ":MM" (so -1 is UTC+1). Returns 0, or -1 (z unchanged). */
int tz_parse_tzone(ars_tz *z, const char *s);

/* Minutes west in force at a UTC time (seconds since 1970). */
long tz_offset_utc(const ars_tz *z, int64_t utc);

/* Minutes west in force at a local wall-clock time (seconds since 1970 of
 * local time). In the repeated hour when DST ends, DST is chosen; a time
 * in the hour skipped when it starts is taken as standard time. */
long tz_offset_local(const ars_tz *z, int64_t local);

#endif
