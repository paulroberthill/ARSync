/* delete.c - --delete when pulling: removing local files the server
 * doesn't have
 *
 * As in rsync (--delete-during), each directory in the transfer is cleaned
 * up when the generator reaches it: whatever it holds that isn't in the
 * file list goes, directories with their contents. Names the filter rules
 * exclude are left alone (unless --delete-excluded), even inside a
 * directory being deleted, which then stays. Deletions go in reverse
 * rsync order, and nothing is deleted if the server had trouble reading
 * its files: an incomplete list would otherwise wipe out good copies.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pull.h"

typedef struct del_ctx {
    pull_state *p;
    char path[FLIST_MAXPATH];   /* local path of the current item */
    char rel[FLIST_MAXPATH];    /* its name relative to the destination */
} del_ctx;

/* --- looking names up in the file list --- */

static const ars_flist *sort_fl;
static int sort_fold;

static int name_cmp(const char *a, const char *b, int fold)
{
    unsigned char ca, cb;

    for (;; a++, b++) {
        ca = (unsigned char)*a;
        cb = (unsigned char)*b;
        if (fold) {
            if (ca >= 'A' && ca <= 'Z')
                ca += 'a' - 'A';
            if (cb >= 'A' && cb <= 'Z')
                cb += 'a' - 'A';
        }
        if (ca != cb || !ca)
            return ca - cb;
    }
}

static int by_name_cmp(const void *a, const void *b)
{
    return name_cmp(flist_name(sort_fl, &sort_fl->files[*(const uint32_t *)a]),
                    flist_name(sort_fl, &sort_fl->files[*(const uint32_t *)b]), sort_fold);
}

/* Builds an index of the list by name (folding case where the local
 * filesystem does). Returns 0 or -1 if out of memory. */
static int index_names(pull_state *p)
{
    size_t i, n = 0;

    if (p->by_name)
        return 0;
    if (!(p->by_name = malloc((p->fl.count ? p->fl.count : 1) * sizeof *p->by_name)))
        return -1;
    for (i = 0; i < p->fl.count; i++)
        if (p->fl.files[i].mode)
            p->by_name[n++] = (uint32_t)i;
    p->by_name_count = n;
    sort_fl = &p->fl;
    sort_fold = p->rules.case_insensitive;
    qsort(p->by_name, n, sizeof *p->by_name, by_name_cmp);
    return 0;
}

/* Whether the list has an entry called name (file or directory alike). */
static int in_list(const pull_state *p, const char *name)
{
    size_t lo = 0, hi = p->by_name_count, mid;
    int c;

    while (lo < hi) {
        mid = (lo + hi) / 2;
        c = name_cmp(name, flist_name(&p->fl, &p->fl.files[p->by_name[mid]]),
                     p->rules.case_insensitive);
        if (c == 0)
            return 1;
        if (c < 0)
            hi = mid;
        else
            lo = mid + 1;
    }
    return 0;
}

/* --- listing and deleting local items --- */

/* Appends "/name" (or ":name"-style for a volume) to buf; returns the old
 * length, or (size_t)-1 if it doesn't fit. */
static size_t push_name(char *buf, size_t size, const char *name)
{
    size_t len = strlen(buf), nlen = strlen(name);
    int sep = len > 0 && buf[len - 1] != '/' && buf[len - 1] != ':';

    if (len + (size_t)sep + nlen >= size)
        return (size_t)-1;
    if (sep)
        buf[len] = '/';
    memcpy(buf + len + sep, name, nlen + 1);
    return len;
}

/* Lists the directory at c->path into l (names only, in rsync order).
 * Unless --delete-excluded, names the rules exclude are left out and
 * counted in *protected. */
static int list_dir(del_ctx *c, ars_flist *l, int *protected)
{
    pull_state *p = c->p;
    int filtered = !p->o->delete_excluded && p->o->filters.count > 0;
    char name[256];
    plat_fileinfo st;
    plat_dir *d;
    size_t len;
    int rc;

    *protected = 0;
    if ((rc = plat_opendir(c->path, &d)) != PLAT_OK)
        return rc;
    while ((rc = plat_readdir(d, name, sizeof name, &st)) == 1) {
        if (filtered) {
            if ((len = push_name(c->rel, sizeof c->rel, name)) == (size_t)-1) {
                (*protected)++;
                continue;
            }
            if (filters_excluded(&p->o->filters, c->rel, st.type == PLAT_T_DIR)) {
                c->rel[len] = '\0';
                (*protected)++;
                continue;
            }
            c->rel[len] = '\0';
        }
        if (flist_add(l, name, st.type == PLAT_T_DIR ? WIRE_S_IFDIR : WIRE_S_IFREG,
                      0, 0, 0) != WIRE_OK) {
            rc = PLAT_ENOMEM;
            break;
        }
    }
    plat_closedir(d);
    if (rc < 0)
        return rc;
    flist_sort_and_clean(l);
    return PLAT_OK;
}

/* Deletes the item at c->path, a directory's contents first. Returns 0 if
 * it's gone, 1 if (some of) it had to stay. log_self is 0 when making way
 * for a file, which rsync doesn't report as a deletion. */
static int delete_item(del_ctx *c, int is_dir, int log_self)
{
    pull_state *p = c->p;
    int left = 0, protected, rc;
    ars_flist l;
    size_t i, plen, rlen;
    const char *name;

    if (is_dir) {
        flist_init(&l);
        if ((rc = list_dir(c, &l, &protected)) != PLAT_OK) {
            pull_file_error(p, "arsync: delete_file: opendir(%s) failed: %s", c->rel, plat_strerror(rc));
            left = 1;
        } else {
            left = protected > 0;
            for (i = l.count; i-- > 0;) {
                name = flist_name(&l, &l.files[i]);
                plen = push_name(c->path, sizeof c->path, name);
                rlen = push_name(c->rel, sizeof c->rel, name);
                if (plen == (size_t)-1 || rlen == (size_t)-1) {
                    left = 1;
                } else if (delete_item(c, WIRE_ISDIR(l.files[i].mode), 1)) {
                    left = 1;
                }
                if (plen != (size_t)-1)
                    c->path[plen] = '\0';
                if (rlen != (size_t)-1)
                    c->rel[rlen] = '\0';
            }
        }
        flist_free(&l);
        if (left && !p->o->dry_run) {
            /* rsync says so on stdout, and it's not an error. */
            printf("cannot delete non-empty directory: %s\n", c->rel);
            return 1;
        }
    }
    if (!p->o->dry_run && (rc = plat_remove(c->path)) != PLAT_OK && rc != PLAT_ENOENT) {
        pull_file_error(p, "arsync: delete_file: %s(%s) failed: %s", is_dir ? "rmdir" : "unlink",
                        c->rel, plat_strerror(rc));
        return 1;
    }
    if (log_self) {
        stats_count(p->st.deleted, is_dir ? WIRE_S_IFDIR : WIRE_S_IFREG);
        if (p->o->verbose)
            printf("deleting %s%s\n", c->rel, is_dir ? "/" : "");
    }
    return 0;
}

/* --- entry points (pull.h) --- */

static int deletion_allowed(pull_state *p)
{
    if (!p->o->delete_mode || p->dest_is_file)
        return 0;
    if ((p->fl.io_error | p->s->remote_io_error) & IOERR_GENERAL) {
        if (!p->delete_warned)
            printf("IO error encountered -- skipping file deletion\n");
        p->delete_warned = 1;
        return 0;
    }
    if (index_names(p) < 0) {
        if (!p->delete_warned)
            pull_file_error(p, "arsync: out of memory -- skipping file deletion");
        p->delete_warned = 1;
        return 0;
    }
    return 1;
}

void delete_in_dir(pull_state *p, size_t ndx, const char *local)
{
    const char *dname = flist_name(&p->fl, &p->fl.files[ndx]), *name;
    int protected, rc;
    size_t i, plen, rlen;
    ars_flist l;
    del_ctx *c;

    if (!deletion_allowed(p))
        return;
    if (!(c = malloc(sizeof *c))) {
        pull_file_error(p, "arsync: out of memory -- skipping file deletion");
        return;
    }
    c->p = p;
    snprintf(c->path, sizeof c->path, "%s", local);
    snprintf(c->rel, sizeof c->rel, "%s", strcmp(dname, ".") == 0 ? "" : dname);

    flist_init(&l);
    if ((rc = list_dir(c, &l, &protected)) != PLAT_OK) {
        pull_file_error(p, "arsync: delete_file: opendir(%s) failed: %s", local, plat_strerror(rc));
    } else {
        for (i = l.count; i-- > 0;) {
            name = flist_name(&l, &l.files[i]);
            plen = push_name(c->path, sizeof c->path, name);
            rlen = push_name(c->rel, sizeof c->rel, name);
            if (plen != (size_t)-1 && rlen != (size_t)-1 && !in_list(p, c->rel))
                delete_item(c, WIRE_ISDIR(l.files[i].mode), 1);
            if (plen != (size_t)-1)
                c->path[plen] = '\0';
            if (rlen != (size_t)-1)
                c->rel[rlen] = '\0';
        }
    }
    flist_free(&l);
    free(c);
}

int delete_for_file(pull_state *p, size_t ndx, const char *local)
{
    del_ctx *c;
    int left;

    if (!(c = malloc(sizeof *c)))
        return PLAT_ENOMEM;
    c->p = p;
    snprintf(c->path, sizeof c->path, "%s", local);
    snprintf(c->rel, sizeof c->rel, "%s", flist_name(&p->fl, &p->fl.files[ndx]));
    left = delete_item(c, 1, 0);
    free(c);
    return left ? PLAT_EIO : PLAT_OK;
}
