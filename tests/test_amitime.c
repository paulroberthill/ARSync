/* test_amitime.c - Unix <-> AmigaDOS DateStamp conversion */
#include "test.h"
#include "amitime.h"

void test_amitime(void)
{
    ars_amidate d;
    long nsec;

    /* The Amiga epoch itself. */
    unix_to_amidate(AMIGA_EPOCH_OFFSET, 0, 0, &d);
    CHECK_EQ(d.days, 0);
    CHECK_EQ(d.minute, 0);
    CHECK_EQ(d.tick, 0);

    /* Earlier times clamp to the epoch. */
    unix_to_amidate(0, 500000000, 0, &d);
    CHECK_EQ(d.days, 0);
    CHECK_EQ(d.minute, 0);
    CHECK_EQ(d.tick, 0);

    /* 2024-01-01 00:00:00.99 UTC = 1704067200.99, 16801 days after 1978-01-01. */
    unix_to_amidate(1704067200, 990000000, 0, &d);
    CHECK_EQ(d.days, 16801);
    CHECK_EQ(d.minute, 0);
    CHECK_EQ(d.tick, 49);
    CHECK_EQ(amidate_to_unix(&d, 0, &nsec), 1704067200);
    CHECK_EQ(nsec, 980000000);   /* tick resolution is 20 ms */

    /* 12:34:56 UTC in UTC+1 (60 minutes east => gmt_offset -60) is 13:34:56 local. */
    unix_to_amidate(1704067200 + 12 * 3600 + 34 * 60 + 56, 0, -60, &d);
    CHECK_EQ(d.days, 16801);
    CHECK_EQ(d.minute, 13 * 60 + 34);
    CHECK_EQ(d.tick, 56 * 50);
    CHECK_EQ(amidate_to_unix(&d, -60, NULL), 1704067200 + 12 * 3600 + 34 * 60 + 56);

    /* West of GMT crossing midnight backwards: 00:30 UTC at UTC-5 is 19:30 the day before. */
    unix_to_amidate(1704067200 + 30 * 60, 0, 300, &d);
    CHECK_EQ(d.days, 16800);
    CHECK_EQ(d.minute, 19 * 60 + 30);
    CHECK_EQ(d.tick, 0);
    CHECK_EQ(amidate_to_unix(&d, 300, NULL), 1704067200 + 30 * 60);
}
