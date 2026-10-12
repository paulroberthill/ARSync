/* flist.c - receiving, storing and ordering the file list */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "flist.h"

void flist_init(ars_flist *fl)
{
    memset(fl, 0, sizeof *fl);
}

void flist_free(ars_flist *fl)
{
    free(fl->files);
    free(fl->names);
    flist_init(fl);
}

int flist_add(ars_flist *fl, const char *path, uint32_t mode, int64_t size,
              int64_t mtime, uint32_t nsec)
{
    size_t len = strlen(path) + 1;
    ars_file *f;

    if (fl->count == fl->cap) {
        size_t cap = fl->cap ? fl->cap * 2 : 256;
        ars_file *p = realloc(fl->files, cap * sizeof *p);

        if (!p)
            return WIRE_ENOMEM;
        fl->files = p;
        fl->cap = cap;
    }
    if (fl->names_cap - fl->names_len < len) {
        size_t cap = fl->names_cap ? fl->names_cap : 4096;
        char *p;

        while (cap - fl->names_len < len)
            cap *= 2;
        if (!(p = realloc(fl->names, cap)))
            return WIRE_ENOMEM;
        fl->names = p;
        fl->names_cap = cap;
    }
    if (fl->names_len > 0xFFFFFFFFu - len)
        return WIRE_ENOMEM;

    f = &fl->files[fl->count++];
    f->name = (uint32_t)fl->names_len;
    f->mode = mode;
    f->size = size;
    f->mtime = mtime;
    f->mtime_nsec = nsec;
    f->xflags = 0;
    memcpy(fl->names + fl->names_len, path, len);
    fl->names_len += len;
    return WIRE_OK;
}

/* ------------------------------------------------------------------ */
/* Ordering                                                            */
/*
 * rsync sorts as if comparing each path as a sequence of (type, byte)
 * pairs, where "item" sorts before "path":
 *   - the directory part and the slash after it are "path" bytes;
 *   - a directory's own name, and a slash after it, are "path" bytes;
 *   - a non-directory's name is "item" bytes;
 *   - every entry ends with an "item" end marker.
 * So within a directory, files come before subdirectories, each
 * subdirectory is followed by its contents, names compare bytewise as if
 * directories ended in '/', and the top directory "." comes first.
 */

enum { SEG_ITEM = 0, SEG_PATH = 1 };

typedef struct name_cursor {
    const char *seg[4];
    unsigned char type[4];
    size_t seglen[4];
    int nseg, i;
    size_t pos;
} name_cursor;

static void cursor_init(name_cursor *c, const char *path, int is_dir)
{
    const char *slash = strrchr(path, '/');
    const char *base = slash ? slash + 1 : path;
    int n = 0;

    if (slash) {
        c->seg[n] = path;
        c->seglen[n] = (size_t)(slash - path) + 1;   /* dirname plus its slash */
        c->type[n++] = SEG_PATH;
    }
    if (!(is_dir && !slash && strcmp(base, ".") == 0)) {
        c->seg[n] = base;
        c->seglen[n] = strlen(base);
        c->type[n++] = is_dir ? SEG_PATH : SEG_ITEM;
        if (is_dir) {
            c->seg[n] = "/";
            c->seglen[n] = 1;
            c->type[n++] = SEG_PATH;
        }
    }
    c->seg[n] = "";
    c->seglen[n] = 0;
    c->type[n++] = SEG_ITEM;     /* end marker */
    c->nseg = n;
    c->i = 0;
    c->pos = 0;
}

/* Next (type, byte); at the end it keeps returning (ITEM, 0). */
static void cursor_next(name_cursor *c, int *type, int *ch)
{
    while (c->pos >= c->seglen[c->i] && c->i < c->nseg - 1) {
        c->i++;
        c->pos = 0;
    }
    *type = c->type[c->i];
    *ch = c->pos < c->seglen[c->i] ? (unsigned char)c->seg[c->i][c->pos++] : 0;
}

int flist_compare(const ars_flist *fl, const ars_file *a, const ars_file *b)
{
    name_cursor ca, cb;
    int ta, tb, cha, chb;

    /* Removed entries sort first, as in rsync. */
    if (!a->mode || !b->mode)
        return (a->mode != 0) - (b->mode != 0);

    cursor_init(&ca, flist_name(fl, a), WIRE_ISDIR(a->mode));
    cursor_init(&cb, flist_name(fl, b), WIRE_ISDIR(b->mode));
    for (;;) {
        cursor_next(&ca, &ta, &cha);
        cursor_next(&cb, &tb, &chb);
        if (ta != tb)
            return ta == SEG_PATH ? 1 : -1;
        if (cha != chb)
            return cha - chb;
        if (cha == 0)
            return 0;
    }
}

/* Stable merge sort, so entries that compare equal keep the order the
 * sender sent them in - which is what the sender's own sort does. */
static void merge_sort(const ars_flist *fl, ars_file *v, ars_file *tmp, size_t n)
{
    size_t mid = n / 2, i = 0, j = mid, k = 0;

    if (n < 2)
        return;
    merge_sort(fl, v, tmp, mid);
    merge_sort(fl, v + mid, tmp, n - mid);
    while (i < mid && j < n)
        tmp[k++] = flist_compare(fl, &v[j], &v[i]) < 0 ? v[j++] : v[i++];
    while (i < mid)
        tmp[k++] = v[i++];
    while (j < n)
        tmp[k++] = v[j++];
    memcpy(v, tmp, n * sizeof *v);
}

/* Finds a non-directory entry named like dir among files[0..end). Entries
 * already removed (mode 0) stay in place and are stepped over. */
static int find_nondir_twin(const ars_flist *fl, size_t end, const ars_file *dir)
{
    ars_file probe = *dir;
    size_t lo = 0, hi = end, mid, m;
    int c;

    probe.mode = WIRE_S_IFREG;
    while (lo < hi) {
        mid = lo + (hi - lo) / 2;
        for (m = mid; m < hi && !fl->files[m].mode; m++)
            ;
        if (m == hi) {
            hi = mid;                /* [mid, hi) holds only removed entries */
            continue;
        }
        c = flist_compare(fl, &probe, &fl->files[m]);
        if (c == 0)
            return (int)m;
        if (c < 0)
            hi = mid;
        else
            lo = m + 1;
    }
    return -1;
}

/* Sorts, then removes duplicate names by clearing their mode (keeping their
 * slot, since indexes must still line up with the sender's). Of two equal
 * names the first is kept, except that a directory beats a non-directory
 * (it may have contents in the list). */
void flist_sort_and_clean(ars_flist *fl)
{
    ars_file *tmp;
    size_t i, prev;
    int j;

    if (fl->count < 2)
        return;
    tmp = malloc(fl->count * sizeof *tmp);
    if (tmp) {
        merge_sort(fl, fl->files, tmp, fl->count);
        free(tmp);
    } else {
        /* Fall back to an in-place insertion sort (also stable). */
        for (i = 1; i < fl->count; i++) {
            ars_file x = fl->files[i];
            size_t k = i;

            while (k > 0 && flist_compare(fl, &x, &fl->files[k - 1]) < 0) {
                fl->files[k] = fl->files[k - 1];
                k--;
            }
            fl->files[k] = x;
        }
    }

    for (prev = 0; prev < fl->count && !fl->files[prev].mode; prev++)
        ;
    for (i = prev + 1; i < fl->count; i++) {
        ars_file *f = &fl->files[i];

        if (!f->mode)
            continue;
        if (flist_compare(fl, f, &fl->files[prev]) == 0)
            j = (int)prev;
        else if (WIRE_ISDIR(f->mode))
            j = find_nondir_twin(fl, prev + 1, f);
        else
            j = -1;

        if (j < 0) {
            prev = i;
        } else if (WIRE_ISDIR(f->mode) && !WIRE_ISDIR(fl->files[j].mode)) {
            fl->files[j].mode = 0;   /* keep the directory */
            prev = i;
        } else {
            f->mode = 0;             /* keep the earlier one */
        }
    }
}

/* ------------------------------------------------------------------ */
/* Receiving                                                           */

/* Rejects names that could escape the destination. */
static int name_is_safe(const char *p)
{
    const char *c;
    size_t n;

    if (*p == '\0' || *p == '/' || p[strlen(p) - 1] == '/')
        return 0;
    if (strcmp(p, ".") == 0)
        return 1;
    for (c = p; *c; c += n + (c[n] == '/')) {
        n = strcspn(c, "/");
        if (n == 0)
            return 0;                        /* "//" */
        if ((n == 1 && c[0] == '.') || (n == 2 && c[0] == '.' && c[1] == '.'))
            return 0;                        /* "." or ".." component */
    }
    return 1;
}

int flist_recv(ars_flist *fl, ars_reader *r, const ars_flist_format *fmt,
               char *err, size_t errlen)
{
    char name[FLIST_MAXPATH];
    size_t namelen = 0, l1, l2;
    uint32_t mode = 0, xflags;
    int64_t mtime = 0, size;
    uint32_t nsec;
    int rc;

    for (;;) {
        if (fmt->varint_flags) {
            xflags = (uint32_t)r_varint(r);
            if (xflags == 0) {
                fl->io_error |= r_varint(r);
                break;
            }
        } else {
            xflags = r_byte(r);
            if (xflags == 0)
                break;
            if (xflags & XMIT_EXTENDED_FLAGS)
                xflags |= (uint32_t)r_byte(r) << 8;
            if (xflags == (XMIT_EXTENDED_FLAGS | XMIT_IO_ERROR_ENDLIST)) {
                if (!fmt->safe_flist) {
                    snprintf(err, errlen, "invalid file-list flags 0x%lx", (unsigned long)xflags);
                    return WIRE_EPROTO;
                }
                fl->io_error |= r_varint(r);
                break;
            }
        }
        if (r->err)
            return r->err;

        /* The name shares a prefix of l1 bytes with the previous one. */
        l1 = xflags & XMIT_SAME_NAME ? r_byte(r) : 0;
        l2 = xflags & XMIT_LONG_NAME ? (size_t)(uint32_t)r_varint(r) : r_byte(r);
        if (r->err)
            return r->err;
        if (l1 > namelen || l2 >= sizeof name - l1) {
            snprintf(err, errlen, "file-list name overflow (prefix %lu, length %lu)",
                     (unsigned long)l1, (unsigned long)l2);
            return WIRE_EPROTO;
        }
        r_bytes(r, name + l1, l2);
        namelen = l1 + l2;
        name[namelen] = '\0';

        size = r_varlong(r, 3);
        if (!(xflags & XMIT_SAME_TIME))
            mtime = r_varlong(r, 4);
        nsec = xflags & XMIT_MOD_NSEC ? (uint32_t)r_varint(r) : 0;
        if (!(xflags & XMIT_SAME_MODE))
            mode = (uint32_t)r_int32(r);
        if (r->err)
            return r->err;

        if (!name_is_safe(name)) {
            snprintf(err, errlen, "unsafe pathname from sender: %s", name);
            return WIRE_EPROTO;
        }
        if (size < 0) {
            snprintf(err, errlen, "negative file size for %s", name);
            return WIRE_EPROTO;
        }
        if (!(mode & WIRE_S_IFMT)) {
            snprintf(err, errlen, "no file type for %s", name);
            return WIRE_EPROTO;
        }
        if ((rc = flist_add(fl, name, mode, size, mtime, nsec)) != WIRE_OK)
            return rc;
        fl->files[fl->count - 1].xflags = xflags;
    }
    if (r->err)
        return r->err;

    flist_sort_and_clean(fl);
    return WIRE_OK;
}

/* ------------------------------------------------------------------ */
/* Sending                                                             */

void flist_send(const ars_flist *fl, ars_writer *w, const ars_flist_format *fmt,
                int protocol, int io_error)
{
    const char *last = "", *name;
    uint32_t prev_mode = 0, xflags;
    int64_t prev_mtime = 0;
    size_t i, l1, l2;

    for (i = 0; i < fl->count; i++) {
        const ars_file *f = &fl->files[i];

        if (!f->mode)
            continue;
        name = flist_name(fl, f);

        /* Owners and groups aren't sent, which rsync marks as "same". */
        xflags = (f->xflags & XMIT_TOP_DIR) | XMIT_SAME_UID | XMIT_SAME_GID;
        if (f->mode == prev_mode)
            xflags |= XMIT_SAME_MODE;
        prev_mode = f->mode;
        if (i > 0 && f->mtime == prev_mtime)
            xflags |= XMIT_SAME_TIME;
        prev_mtime = f->mtime;
        if (f->mtime_nsec && protocol >= 31)
            xflags |= XMIT_MOD_NSEC;

        for (l1 = 0; last[l1] && name[l1] == last[l1] && l1 < 255; l1++)
            ;
        l2 = strlen(name + l1);
        if (l1 > 0)
            xflags |= XMIT_SAME_NAME;
        if (l2 > 255)
            xflags |= XMIT_LONG_NAME;

        if (fmt->varint_flags) {
            w_varint(w, (int32_t)xflags);
        } else {
            /* Older encoding: one byte, or two with XMIT_EXTENDED_FLAGS.
             * (The uid/gid bits keep the value from ever being 0, which
             * would end the list.) */
            if (xflags & 0xFF00) {
                xflags |= XMIT_EXTENDED_FLAGS;
                w_uint16(w, (uint16_t)xflags);
            } else {
                w_byte(w, (uint8_t)xflags);
            }
        }
        if (xflags & XMIT_SAME_NAME)
            w_byte(w, (uint8_t)l1);
        if (xflags & XMIT_LONG_NAME)
            w_varint(w, (int32_t)l2);
        else
            w_byte(w, (uint8_t)l2);
        w_bytes(w, name + l1, l2);

        w_varlong(w, f->size, 3);
        if (!(xflags & XMIT_SAME_TIME))
            w_varlong(w, f->mtime, 4);
        if (xflags & XMIT_MOD_NSEC)
            w_varint(w, (int32_t)f->mtime_nsec);
        if (!(xflags & XMIT_SAME_MODE))
            w_int32(w, (int32_t)f->mode);
        last = name;
    }

    if (fmt->varint_flags) {
        w_varint(w, 0);
        w_varint(w, io_error);
    } else if (io_error && fmt->safe_flist) {
        w_uint16(w, XMIT_EXTENDED_FLAGS | XMIT_IO_ERROR_ENDLIST);
        w_varint(w, io_error);
    } else {
        w_byte(w, 0);
    }
}
