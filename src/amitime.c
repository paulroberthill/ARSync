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
