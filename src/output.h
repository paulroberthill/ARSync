/* output.h - rsync-compatible formatting of numbers, modes, times, listings */
#ifndef OUTPUT_H
#define OUTPUT_H

#include "flist.h"

/* 1234567 -> "1,234,567". buf needs 32 bytes. */
const char *comma_num(int64_t n, char *buf);
/* A value in hundredths, e.g. 19000 -> "190.00", with grouping. buf: 40 bytes. */
const char *comma_num_frac2(int64_t hundredths, char *buf);
/* "drwxr-xr-x" style, from a wire mode. buf needs 11 bytes. */
const char *permstring(uint32_t mode, char *buf);
/* "2026/10/12 14:00:00" in local time. buf needs 20 bytes. */
const char *timestring(int64_t t, char *buf);
/* One --list-only line (without newline), as rsync prints it. */
void format_list_entry(const ars_flist *fl, const ars_file *f, char *buf, size_t bufsize);

#endif
