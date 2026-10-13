/* scan.c - building a file list from local files (the sending side)
 *
 * Breadth-first, using the list itself as the queue: each directory entry
 * is read in turn and its children appended. No recursion, so deep trees
 * don't eat the stack, and only one directory is open at a time (one Lock
 * on AmigaOS).
 */
#include <stdio.h>
#include <string.h>

#include "scan.h"
#include "plat.h"

static void join(char *buf, size_t bufsize, const char *dir, const char *name)
{
    size_t len = strlen(dir);

    if (len == 0)
        snprintf(buf, bufsize, "%s", name);
    else if (dir[len - 1] == '/' || dir[len - 1] == ':')
        snprintf(buf, bufsize, "%s%s", dir, name);
    else
        snprintf(buf, bufsize, "%s/%s", dir, name);
}

void scan_local_path(const ars_flist *fl, const ars_file *f, const char *base,
                     char *buf, size_t bufsize)
{
    const char *name = flist_name(fl, f);

    if (strcmp(name, ".") == 0)
        snprintf(buf, bufsize, "%s", *base ? base : ".");
    else
        join(buf, bufsize, base, name);
}

static uint32_t wire_mode(const plat_fileinfo *st)
{
    switch (st->type) {
    case PLAT_T_DIR:
        return WIRE_S_IFDIR | (st->perms & 07777);
    case PLAT_T_LINK:
        return WIRE_S_IFLNK | 0777;
    default:
        return WIRE_S_IFREG | (st->perms & 07777);
    }
}

static int add(ars_flist *fl, const char *name, const plat_fileinfo *st, uint32_t xflags)
{
    int rc = flist_add(fl, name, wire_mode(st), st->type == PLAT_T_DIR ? 0 : st->size,
                       st->mtime, (uint32_t)st->mtime_nsec);

    if (rc == WIRE_OK)
        fl->files[fl->count - 1].xflags = xflags;
    return rc;
}

/* Appends the contents of directory entry ndx. */
static int scan_dir(ars_flist *fl, size_t ndx, const ars_filters *filters, const char *base,
                    int *io_error)
{
    char path[FLIST_MAXPATH], child[FLIST_MAXPATH], name[256];
    const char *dirname;
    plat_fileinfo st;
    plat_dir *d;
    int rc;

    scan_local_path(fl, &fl->files[ndx], base, path, sizeof path);
    if ((rc = plat_opendir(path, &d)) != PLAT_OK) {
        fprintf(stderr, "arsync: opendir \"%s\" failed: %s\n", path, plat_strerror(rc));
        *io_error |= IOERR_GENERAL;
        return WIRE_OK;
    }
    while ((rc = plat_readdir(d, name, sizeof name, &st)) == 1) {
        /* The name pool may move as entries are added, so look it up
         * afresh each time. */
        dirname = flist_name(fl, &fl->files[ndx]);
        if (strcmp(dirname, ".") == 0)
            snprintf(child, sizeof child, "%s", name);
        else if ((size_t)snprintf(child, sizeof child, "%s/%s", dirname, name) >= sizeof child) {
            fprintf(stderr, "arsync: skipping \"%s/%s\": path too long\n", dirname, name);
            *io_error |= IOERR_GENERAL;
            continue;
        }
        /* Symlinks go in the list, as rsync's sender does without -l; the
         * receiver reports and skips them. Devices and the like don't. */
        if (filters_excluded(filters, child, st.type == PLAT_T_DIR))
            continue;
        if (st.type == PLAT_T_OTHER) {
            printf("skipping non-regular file \"%s\"\n", child);
            continue;
        }
        if ((rc = add(fl, child, &st, 0)) != WIRE_OK) {
            plat_closedir(d);
            return rc;
        }
    }
    plat_closedir(d);
    if (rc < 0) {
        fprintf(stderr, "arsync: readdir \"%s\" failed: %s\n", path, plat_strerror(rc));
        *io_error |= IOERR_GENERAL;
    }
    return WIRE_OK;
}

int scan_source(ars_flist *fl, const char *src, int recursive, const ars_filters *filters,
                char *base, size_t basesize, int *io_error)
{
    char path[FLIST_MAXPATH];
    const char *name, *sep;
    plat_fileinfo st;
    size_t len, i;
    int contents, rc;

    snprintf(path, sizeof path, "%s", *src ? src : ".");
    len = strlen(path);
    /* "dir/" means its contents; so do "Vol:" and "." */
    contents = path[len - 1] == '/' || path[len - 1] == ':' || strcmp(path, ".") == 0;
    while (len > 1 && path[len - 1] == '/')
        path[--len] = '\0';

    if ((rc = plat_stat(path, &st)) != PLAT_OK) {
        fprintf(stderr, "arsync: link_stat \"%s\" failed: %s\n", path, plat_strerror(rc));
        *io_error |= IOERR_GENERAL;
        base[0] = '\0';
        return WIRE_OK;
    }

    if (st.type == PLAT_T_DIR && contents) {
        snprintf(base, basesize, "%s", path);
        name = ".";
    } else {
        /* Split "a/b/c" into base "a/b" and name "c" ("Vol:c": "Vol:", "c"). */
        sep = strrchr(path, '/');
        if (!sep || (strrchr(path, ':') && strrchr(path, ':') > sep))
            sep = strrchr(path, ':');
        if (sep == path && *sep == '/') {
            snprintf(base, basesize, "/");
            name = sep + 1;
        } else if (sep) {
            snprintf(base, basesize, "%.*s", (int)(sep - path + (*sep == ':')), path);
            name = sep + 1;
        } else {
            base[0] = '\0';
            name = path;
        }
    }

    if (strcmp(name, ".") != 0 && filters_excluded(filters, name, st.type == PLAT_T_DIR))
        return WIRE_OK;
    if (st.type == PLAT_T_DIR && !recursive) {
        printf("skipping directory %s\n", name);
        return WIRE_OK;
    }
    if (st.type == PLAT_T_OTHER) {
        printf("skipping non-regular file \"%s\"\n", name);
        return WIRE_OK;
    }
    if ((rc = add(fl, name, &st, st.type == PLAT_T_DIR ? XMIT_TOP_DIR : 0)) != WIRE_OK)
        return rc;

    for (i = 0; i < fl->count; i++) {
        if (WIRE_ISDIR(fl->files[i].mode) && (rc = scan_dir(fl, i, filters, base, io_error)) != WIRE_OK)
            return rc;
        if (plat_break_pending())
            break;
    }
    return WIRE_OK;
}
