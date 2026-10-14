/* test_tz.c - POSIX TZ rules and ENV:TZONE */
#include "test.h"
#include "tz.h"

/* 2026-03-29 01:00 UTC: BST starts; 2026-10-25 01:00 UTC: it ends. */
#define UK_START 1774746000LL
#define UK_END   1792890000LL

static void test_parse(void)
{
    ars_tz z;

    CHECK_EQ(tz_parse(&z, "GMT0BST,M3.5.0/1,M10.5.0"), 0);
    CHECK_EQ(z.std_off, 0);
    CHECK_EQ(z.dst_off, -3600);
    CHECK(z.has_dst);
    CHECK_EQ(z.start.time, 3600);
    CHECK_EQ(z.end.time, 7200);

    CHECK_EQ(tz_parse(&z, "EST5EDT,M3.2.0,M11.1.0"), 0);
    CHECK_EQ(z.std_off, 5 * 3600);
    CHECK_EQ(z.dst_off, 4 * 3600);

    CHECK_EQ(tz_parse(&z, "<+0530>-5:30"), 0);
    CHECK_EQ(z.std_off, -(5 * 3600 + 30 * 60));
    CHECK(!z.has_dst);

    CHECK_EQ(tz_parse(&z, "CET-1CEST-2,J60/2:30,300/-1"), 0);
    CHECK_EQ(z.dst_off, -7200);
    CHECK_EQ(z.start.kind, 'J');
    CHECK_EQ(z.start.time, 2 * 3600 + 30 * 60);
    CHECK_EQ(z.end.kind, 'D');
    CHECK_EQ(z.end.time, -3600);

    /* Not valid: z must stay as it was. */
    tz_fixed(&z, 123);
    CHECK_EQ(tz_parse(&z, ""), -1);
    CHECK_EQ(tz_parse(&z, "GMT"), -1);
    CHECK_EQ(tz_parse(&z, "GMT0BST"), -1);              /* no rule: not guessed */
    CHECK_EQ(tz_parse(&z, "-1"), -1);
    CHECK_EQ(tz_parse(&z, "GMT0BST,M3.5.0/1"), -1);
    CHECK_EQ(tz_parse(&z, "GMT0BST,M13.5.0,M10.5.0"), -1);
    CHECK_EQ(tz_parse(&z, "GMT0BST,M3.5.0,M10.5.0x"), -1);
    CHECK_EQ(z.std_off, 123 * 60);

    CHECK_EQ(tz_parse_tzone(&z, "-1"), 0);
    CHECK_EQ(tz_offset_utc(&z, UK_START), -60);
    CHECK_EQ(tz_parse_tzone(&z, "5"), 0);
    CHECK_EQ(tz_offset_utc(&z, 0), 300);
    CHECK_EQ(tz_parse_tzone(&z, "-5:30"), 0);
    CHECK_EQ(tz_offset_utc(&z, 0), -330);
    CHECK_EQ(tz_parse_tzone(&z, "x"), -1);
    CHECK_EQ(tz_parse_tzone(&z, "1x"), -1);
}

static void test_uk(void)
{
    ars_tz z;
    int64_t local;

    tz_parse(&z, "GMT0BST,M3.5.0/1,M10.5.0");

    CHECK_EQ(tz_offset_utc(&z, UK_START - 1), 0);
    CHECK_EQ(tz_offset_utc(&z, UK_START), -60);
    CHECK_EQ(tz_offset_utc(&z, UK_END - 1), -60);
    CHECK_EQ(tz_offset_utc(&z, UK_END), 0);
    CHECK_EQ(tz_offset_utc(&z, 1767225600LL), 0);       /* 2026-01-01 */

    /* The report: 2026-09-08 14:55:45 BST is 13:55:45 UTC. */
    local = 1788879345LL;                                /* 2026-09-08 14:55:45 as if UTC */
    CHECK_EQ(tz_offset_local(&z, local), -60);
    CHECK_EQ(local + tz_offset_local(&z, local) * 60, 1788875745LL);

    /* Round trips away from the changes. */
    CHECK_EQ(tz_offset_local(&z, 1767225600LL + 12 * 3600), 0);

    /* 01:30 on 29 March doesn't exist: standard time. */
    CHECK_EQ(tz_offset_local(&z, UK_START + 1800), 0);
    /* 01:30 on 25 October happens twice: the first (BST) is chosen. */
    CHECK_EQ(tz_offset_local(&z, UK_END + 1800), -60);
    CHECK_EQ(tz_offset_local(&z, UK_END + 3600), 0);

    /* Years past 2038 and the 68k's 64-bit arithmetic. */
    CHECK_EQ(tz_offset_utc(&z, 4102444800LL + 180 * 86400), -60);   /* 2100-06-30 */
    CHECK_EQ(tz_offset_utc(&z, 4102444800LL), 0);                   /* 2100-01-01 */
}

static void test_south(void)
{
    ars_tz z;

    /* Sydney: AEDT from the first Sunday in October to the first in April. */
    tz_parse(&z, "AEST-10AEDT,M10.1.0,M4.1.0/3");
    CHECK_EQ(tz_offset_utc(&z, 1767225600LL), -660);           /* 2026-01-01: summer */
    CHECK_EQ(tz_offset_utc(&z, 1767225600LL + 180 * 86400), -600); /* 2026-06-30 */
    /* 2026-04-05 03:00 AEDT = 2026-04-04 16:00 UTC. */
    CHECK_EQ(tz_offset_utc(&z, 1775318400LL - 1), -660);
    CHECK_EQ(tz_offset_utc(&z, 1775318400LL), -600);
}

static void test_last_week(void)
{
    ars_tz z;

    /* M2.5.0 in 2026 (no 5th Sunday) is the 4th, 22 Feb; in a leap year
     * J60 is 1 March, not 29 Feb. */
    tz_parse(&z, "XXX0YYY,M2.5.0/0,J60/0");
    CHECK_EQ(tz_offset_utc(&z, 1771718400LL - 1), 0);     /* 2026-02-22 00:00 UTC */
    CHECK_EQ(tz_offset_utc(&z, 1771718400LL), -60);
    tz_parse(&z, "XXX0YYY,J59/0,J60/0");                  /* 28 Feb .. 1 Mar */
    CHECK_EQ(tz_offset_utc(&z, 1709164800LL), -60);       /* 2024-02-29 00:00 UTC */
    /* It ends at 1 Mar 00:00 in DST, i.e. 29 Feb 23:00 UTC. */
    CHECK_EQ(tz_offset_utc(&z, 1709251200LL - 3601), -60);
    CHECK_EQ(tz_offset_utc(&z, 1709251200LL - 3600), 0);
}

void test_tz_suite(void)
{
    test_parse();
    test_uk();
    test_south();
    test_last_week();
}
