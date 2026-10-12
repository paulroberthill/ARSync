/* scan.h - building a file list from local files (the sending side) */
#ifndef SCAN_H
#define SCAN_H

#include "flist.h"

/* Builds the list for one source, the way rsync's sender does:
 *   "dir/" or "Vol:" (trailing '/' or ':') sends the contents, as ".";
 *   "dir" sends the directory itself, as "dir";
 *   a file sends just its name.
 * Directories are only descended into with recursive set. base receives
 * the directory names in the list are relative to. Problems are reported
 * on stderr and recorded in *io_error (IOERR_*). Returns WIRE_OK or
 * WIRE_ENOMEM; the list is left unsorted. */
int scan_source(ars_flist *fl, const char *src, int recursive,
                char *base, size_t basesize, int *io_error);

/* The local path of entry f: base joined with its name ("." is base). */
void scan_local_path(const ars_flist *fl, const ars_file *f, const char *base,
                     char *buf, size_t bufsize);

#endif
