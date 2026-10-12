/* amitime.c - conversion between Unix time and AmigaDOS DateStamps */
#include "amitime.h"

void unix_to_amidate(int64_t secs, long nsec, long gmt_offset, ars_amidate *d)
{
    int64_t local = secs - AMIGA_EPOCH_OFFSET - (int64_t)gmt_offset * 60;

    if (local < 0) {
        local = 0;
        nsec = 0;
    }
    d->days = (long)(local / 86400);
    d->minute = (long)(local % 86400 / 60);
    d->tick = (long)(local % 60) * AMIGA_TICKS_PER_SECOND
              + nsec / (1000000000L / AMIGA_TICKS_PER_SECOND);
}

int64_t amidate_to_unix(const ars_amidate *d, long gmt_offset, long *nsec_out)
{
    int64_t secs = (int64_t)d->days * 86400 + (int64_t)d->minute * 60
                   + d->tick / AMIGA_TICKS_PER_SECOND;

    if (nsec_out)
        *nsec_out = d->tick % AMIGA_TICKS_PER_SECOND * (1000000000L / AMIGA_TICKS_PER_SECOND);
    return secs + AMIGA_EPOCH_OFFSET + (int64_t)gmt_offset * 60;
}

/* Days since 1970-01-01 to a proleptic Gregorian date, counting in 400-year
 * eras of 146097 days that start on 1 March, so leap days fall at the end
 * of each era-year. */
void secs_to_civil(int64_t secs, ars_tm *tm)
{
    int64_t days = secs / 86400, rem = secs % 86400, era, doe, yoe, doy, mp;

    if (rem < 0) {
        rem += 86400;
        days--;
    }
    tm->hour = (int)(rem / 3600);
    tm->minute = (int)(rem % 3600 / 60);
    tm->second = (int)(rem % 60);

    days += 719468;                       /* shift epoch to 0000-03-01 */
    era = (days >= 0 ? days : days - 146096) / 146097;
    doe = days - era * 146097;            /* day of era, 0..146096 */
    yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    mp = (5 * doy + 2) / 153;             /* March-based month, 0..11 */
    tm->day = (int)(doy - (153 * mp + 2) / 5 + 1);
    tm->month = (int)(mp < 10 ? mp + 3 : mp - 9);
    tm->year = (int)(yoe + era * 400 + (tm->month <= 2));
}
