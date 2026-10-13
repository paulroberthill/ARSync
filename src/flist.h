/* flist.h - receiving, storing and ordering the file list */
#ifndef FLIST_H
#define FLIST_H

#include <string.h>

#include "wire.h"

/* Unix mode bits as they travel on the wire (independent of the host). */
#define WIRE_S_IFMT 0170000
#define WIRE_S_IFSOCK 0140000
#define WIRE_S_IFLNK 0120000
#define WIRE_S_IFREG 0100000
#define WIRE_S_IFBLK 0060000
#define WIRE_S_IFDIR 0040000
#define WIRE_S_IFCHR 0020000
#define WIRE_S_IFIFO 0010000
#define WIRE_S_ISUID 04000
#define WIRE_S_ISGID 02000
#define WIRE_S_ISVTX 01000

#define WIRE_ISDIR(m) (((m) & WIRE_S_IFMT) == WIRE_S_IFDIR)
#define WIRE_ISREG(m) (((m) & WIRE_S_IFMT) == WIRE_S_IFREG)

/* File-list entry flags on the wire (XMIT_*), as far as we use them. */
#define XMIT_TOP_DIR (1 << 0)
#define XMIT_SAME_MODE (1 << 1)
#define XMIT_EXTENDED_FLAGS (1 << 2)
#define XMIT_SAME_UID (1 << 3)
#define XMIT_SAME_GID (1 << 4)
#define XMIT_SAME_NAME (1 << 5)
#define XMIT_LONG_NAME (1 << 6)
#define XMIT_SAME_TIME (1 << 7)
#define XMIT_NO_CONTENT_DIR (1 << 8)
#define XMIT_HLINKED (1 << 9)
#define XMIT_IO_ERROR_ENDLIST (1 << 12)
#define XMIT_MOD_NSEC (1 << 13)

/* I/O error bits a sender reports at the end of the list. */
#define IOERR_GENERAL (1 << 0)
#define IOERR_VANISHED (1 << 1)

#define FLIST_MAXPATH 1024

/* Item flags exchanged with each request and reply (ITEM_*). */
#define ITEM_REPORT_ATIME (1 << 0)
#define ITEM_REPORT_CHANGE (1 << 1)
#define ITEM_REPORT_SIZE (1 << 2)
#define ITEM_REPORT_TIME (1 << 3)
#define ITEM_REPORT_PERMS (1 << 4)
#define ITEM_REPORT_OWNER (1 << 5)
#define ITEM_REPORT_GROUP (1 << 6)
#define ITEM_REPORT_ACL (1 << 7)
#define ITEM_REPORT_XATTR (1 << 8)
#define ITEM_REPORT_CRTIME (1 << 10)
#define ITEM_BASIS_TYPE_FOLLOWS (1 << 11)
#define ITEM_XNAME_FOLLOWS (1 << 12)
#define ITEM_IS_NEW (1 << 13)
#define ITEM_LOCAL_CHANGE (1 << 14)
#define ITEM_TRANSFER (1 << 15)
/* Local only, never sent: */
#define ITEM_MISSING_DATA (1L << 16)
#define ITEM_DELETED (1L << 17)
/* Flags that make an item worth reporting. */
#define ITEM_SIGNIFICANT (0xFFFF & ~(ITEM_BASIS_TYPE_FOLLOWS | ITEM_XNAME_FOLLOWS | ITEM_LOCAL_CHANGE))

typedef struct ars_file {
    uint32_t name;      /* offset of the full path in the name pool */
    uint32_t mode;      /* wire mode; 0 = removed duplicate */
    int64_t size;
    int64_t mtime;
    uint32_t mtime_nsec;
    uint32_t xflags;    /* XMIT_* flags as received */
} ars_file;

typedef struct ars_flist {
    ars_file *files;    /* in sorted order; index = protocol index */
    size_t count, cap;
    char *names;        /* pool of NUL-terminated paths */
    size_t names_len, names_cap;
    /* -c: bytes of whole-file checksum sent with each regular file, kept
     * in the pool after its name (see flist_sum()); 0 = none. Set before
     * adding entries. */
    int sum_len;
    int io_error;       /* IOERR_* bits reported by the sender */
} ars_flist;

/* How the sender encodes entries, from the negotiated session. */
typedef struct ars_flist_format {
    int varint_flags;   /* CF_VARINT_FLIST_FLAGS */
    int safe_flist;     /* CF_SAFE_FLIST or protocol >= 31 */
} ars_flist_format;

void flist_init(ars_flist *fl);
void flist_free(ars_flist *fl);

/* Reads the whole list from r, then sorts it and drops duplicates exactly
 * as the sender does, so positions match the sender's indexes.
 * Returns WIRE_OK, a WIRE_E* stream error (see r->err), or WIRE_EPROTO
 * with a message in err for bad entries. */
int flist_recv(ars_flist *fl, ars_reader *r, const ars_flist_format *fmt,
               char *err, size_t errlen);

/* Sends the list (in its current order, normally sorted) as a sender
 * does, ending with io_error. protocol decides whether nanoseconds go. */
void flist_send(const ars_flist *fl, ars_writer *w, const ars_flist_format *fmt,
                int protocol, int io_error);

static inline const char *flist_name(const ars_flist *fl, const ars_file *f)
{
    return fl->names + f->name;
}

/* The entry's whole-file checksum (fl->sum_len bytes; zeros if not set). */
static inline uint8_t *flist_sum(const ars_flist *fl, const ars_file *f)
{
    const char *name = fl->names + f->name;

    return (uint8_t *)name + strlen(name) + 1;
}

/* rsync's file-list order; exported for tests. */
int flist_compare(const ars_flist *fl, const ars_file *a, const ars_file *b);
void flist_sort_and_clean(ars_flist *fl);

/* Adds an entry directly (tests, and later the sending side). */
int flist_add(ars_flist *fl, const char *path, uint32_t mode, int64_t size,
              int64_t mtime, uint32_t nsec);

#endif
