/* pull.h - state shared by the generator and receiver halves of a pull
 *
 * rsync runs these as two processes; arsync runs them as one, alternating:
 * the generator walks the file list and writes requests, the receiver reads
 * the server's replies (one per request) and writes files. See client.c.
 */
#ifndef PULL_H
#define PULL_H

#include "flist.h"
#include "fsmap.h"
#include "progress.h"
#include "plat.h"
#include "session.h"

/* Item flags exchanged with each request and reply (ITEM_*). */
#define ITEM_REPORT_ATIME (1 << 0)
#define ITEM_REPORT_CHANGE (1 << 1)
#define ITEM_REPORT_SIZE (1 << 2)
#define ITEM_REPORT_TIME (1 << 3)
#define ITEM_REPORT_PERMS (1 << 4)
#define ITEM_BASIS_TYPE_FOLLOWS (1 << 11)
#define ITEM_XNAME_FOLLOWS (1 << 12)
#define ITEM_IS_NEW (1 << 13)
#define ITEM_LOCAL_CHANGE (1 << 14)
#define ITEM_TRANSFER (1 << 15)
/* Flags that make an item worth reporting. */
#define ITEM_SIGNIFICANT (0xFFFF & ~(ITEM_BASIS_TYPE_FOLLOWS | ITEM_XNAME_FOLLOWS | ITEM_LOCAL_CHANGE))

#define MAX_BLOCK_SIZE (1L << 17)
#define CHUNK_SIZE 32768

/* How many requests may be awaiting replies. Requests are tiny, so this
 * mostly bounds how far the generator runs ahead of the receiver. */
#define PULL_WINDOW 32

/* Per-file state, alongside the file list. */
#define FS_REDO 1           /* failed verification once; asked for again */
#define FS_SKIPPED 2        /* not attempted (e.g. parent dir failed) */
#define FS_CASE_CLASH 4     /* same name as an earlier entry, ignoring case */

typedef struct pull_state {
    ars_session *s;
    const ars_options *o;
    ars_flist fl;
    uint8_t *fstate;            /* FS_* per file */
    ars_ndx_state gen_ndx;      /* our outgoing indexes */
    ars_ndx_state recv_ndx;     /* the server's incoming indexes */

    /* Where files go: a directory the list is relative to, or (when a
     * single file is fetched) one destination path. */
    int listing;                /* --list-only: no requests at all */
    char dest[FLIST_MAXPATH];
    int dest_is_file;
    int dest_created;           /* we made the destination directory */

    size_t gen_next;            /* next file-list index for the generator */
    int outstanding;            /* requests sent, replies not yet seen */
    int redoing;                /* receiver is in the redo phase */
    int32_t *redo;              /* indexes to request again */
    size_t redo_count;

    char failed_dir[FLIST_MAXPATH];  /* contents of this dir are skipped */
    int xfer_error;             /* some file failed: exit code 23 */
    int interrupted;            /* Ctrl-C seen and reported */
    ars_fsrules rules;          /* what the destination can hold */
    long xfr_count;             /* files transferred, for --progress */
    ars_progress progress;

    char tmp[FLIST_MAXPATH];    /* temp file being written, "" if none */
    uint8_t *buf;               /* CHUNK_SIZE bytes for file data */
    uint8_t *block;             /* MAX_BLOCK_SIZE bytes for the generator */

    /* The receiver's view of the old file a delta refers to. */
    plat_file *basis;
    int64_t basis_size;
    uint8_t *basis_buf;         /* BASIS_CACHE bytes */
    int64_t basis_off;          /* file offset of basis_buf[0] */
    size_t basis_len;           /* valid bytes in basis_buf */
} pull_state;

/* Old-file data is read in windows this big (at least two blocks). */
#define BASIS_CACHE (2 * MAX_BLOCK_SIZE)

/* Builds the local path for file-list entry ndx into buf. */
void pull_local_path(const pull_state *p, size_t ndx, char *buf, size_t bufsize);

/* Reports a problem with one file (rsync's FERROR_XFER): printed, and the
 * run ends with exit code 23. */
void pull_file_error(pull_state *p, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

/* Returns RERR_SIGNAL (once reported) if Ctrl-C has been pressed. */
int pull_check_break(pull_state *p);

/* generator.c: handles one file-list entry, possibly writing a request.
 * Returns RERR_OK or a fatal exit code. */
int gen_entry(pull_state *p, size_t ndx);
/* Writes a request for a file again (redo phase). */
void gen_redo(pull_state *p, int32_t ndx);

/* receiver.c: reads and handles one reply. Sets *done when it was
 * NDX_DONE instead. Unless until_done is set, returns without reading
 * anything once no replies are outstanding (files the server couldn't
 * send don't get one). Returns RERR_OK or a fatal exit code. */
int recv_reply(pull_state *p, int until_done, int *done);
/* Removes a half-written temp file (on errors and Ctrl-C). */
void recv_cleanup(pull_state *p);

#endif
