/* amitime.h - conversion between Unix time and AmigaDOS DateStamps
 *
 * AmigaDOS stamps count days, minutes and ticks (1/50 s) from 1978-01-01 in
 * local time. gmt_offset is in minutes west of Greenwich, as in
 * locale.library's loc_GMTOffset, so local time = UTC - gmt_offset minutes.
 */
#ifndef AMITIME_H
#define AMITIME_H

#include "arsync.h"

#define AMIGA_EPOCH_OFFSET 252460800L  /* seconds from 1970-01-01 to 1978-01-01 */
#define AMIGA_TICKS_PER_SECOND 50

typedef struct ars_amidate {
    long days;
    long minute;
    long tick;
} ars_amidate;

typedef struct ars_tm {
    int year;      /* e.g. 2026 */
    int month;     /* 1-12 */
    int day;       /* 1-31 */
    int hour, minute, second;
} ars_tm;

/* Splits seconds since 1970-01-01 (in whatever zone they're counted) into a
 * calendar date and time. Works for negative values too. */
void secs_to_civil(int64_t secs, ars_tm *tm);

/* Times before the Amiga epoch are clamped to 1978-01-01 00:00:00. */
void unix_to_amidate(int64_t secs, long nsec, long gmt_offset, ars_amidate *d);
int64_t amidate_to_unix(const ars_amidate *d, long gmt_offset, long *nsec_out);

#endif
