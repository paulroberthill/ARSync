/* progress.h - rsync-style --progress lines */
#ifndef PROGRESS_H
#define PROGRESS_H

#include "arsync.h"

typedef struct ars_progress {
    int64_t size;
    uint32_t start_ms;
    uint32_t last_ms;
    int printed;
} ars_progress;

void progress_start(ars_progress *pr, int64_t size);
/* Redraws the line at most twice a second. */
void progress_update(ars_progress *pr, int64_t done);
/* Final line: elapsed time and "(xfr#N, to-chk=M/T)". */
void progress_end(ars_progress *pr, int64_t done, long xfr, long to_check, long total);

/* "  123.45kB/s" style rate, from bytes over milliseconds. buf: 24 bytes. */
const char *progress_rate(int64_t bytes, uint32_t ms, char *buf);

#endif
