/* progress.c - rsync-style --progress lines
 *
 *          28,672  45%  123.45kB/s    0:00:12
 *         100,000 100%   95.37MB/s    0:00:01 (xfr#2, to-chk=3/7)
 *
 * All integer arithmetic, so the Amiga build needs no floating point.
 */
#include <stdio.h>

#include "progress.h"
#include "output.h"
#include "plat.h"

#define UPDATE_MS 500

const char *progress_rate(int64_t bytes, uint32_t ms, char *buf)
{
    /* Hundredths of kB/s (1 kB = 1024 bytes), as rsync shows it. */
    int64_t rate = bytes * 100000 / (ms ? ms : 1) / 1024;
    const char *units = "kB/s";

    if (rate > (int64_t)1024 * 1024 * 100) {
        rate /= 1024 * 1024;
        units = "GB/s";
    } else if (rate > 1024 * 100) {
        rate /= 1024;
        units = "MB/s";
    }
    snprintf(buf, 24, "%4ld.%02ld%s", (long)(rate / 100), (long)(rate % 100), units);
    return buf;
}

static void print_line(int64_t done, int64_t size, const char *rate, uint32_t secs, const char *eol)
{
    char num[32];
    int pct = done >= size ? 100 : (int)(done * 100 / size);

    printf("\r%15s %3d%% %s %4lu:%02lu:%02lu%s", comma_num(done, num), pct, rate,
           (unsigned long)(secs / 3600), (unsigned long)(secs / 60 % 60),
           (unsigned long)(secs % 60), eol);
    fflush(stdout);
}

void progress_start(ars_progress *pr, int64_t size)
{
    pr->size = size;
    pr->start_ms = pr->last_ms = plat_millis();
    pr->printed = 0;
}

void progress_update(ars_progress *pr, int64_t done)
{
    uint32_t now = plat_millis(), elapsed = now - pr->start_ms;
    char rate[24];
    uint32_t remain = 0;

    if (pr->printed && now - pr->last_ms < UPDATE_MS)
        return;
    pr->last_ms = now;
    pr->printed = 1;
    if (done > 0 && elapsed > 0)
        remain = (uint32_t)((pr->size - done) * (int64_t)elapsed / done / 1000);
    print_line(done, pr->size, progress_rate(done, elapsed, rate), remain, "  ");
}

void progress_end(ars_progress *pr, int64_t done, long xfr, long to_check, long total)
{
    uint32_t elapsed = plat_millis() - pr->start_ms;
    char rate[24], eol[64];

    snprintf(eol, sizeof eol, " (xfr#%ld, to-chk=%ld/%ld)\n", xfr, to_check, total);
    print_line(done, pr->size, progress_rate(done, elapsed, rate), elapsed / 1000, eol);
}
