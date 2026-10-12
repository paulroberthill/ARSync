/* generator.c - decides what to fetch and writes the requests
 *
 * For each file-list entry: directories are created locally; regular files
 * that already match (same size and modification time, rsync's "quick
 * check") are skipped; anything else is requested. Items that change
 * without a transfer (a new directory, say) are still reported to the
 * server, which echoes them back so the receiver can log them in order,
 * just as rsync does.
 */
#include <stdio.h>
#include <string.h>

#include "pull.h"
#include "plat.h"

static void write_item(pull_state *p, int32_t ndx, uint16_t iflags)
{
    w_ndx(&p->s->out, &p->gen_ndx, ndx);
    w_uint16(&p->s->out, iflags);
    p->outstanding++;
}

/* Every request in arsync is for the whole file: a checksum header with no
 * blocks tells the server to send the file as literal data. */
static void write_empty_sum_head(pull_state *p)
{
    w_int32(&p->s->out, 0);   /* block count */
    w_int32(&p->s->out, 0);   /* block length */
    w_int32(&p->s->out, 0);   /* strong checksum length */
    w_int32(&p->s->out, 0);   /* last block length */
}

static int is_below(const char *dir, const char *name)
{
    size_t len = strlen(dir);

    return strncmp(name, dir, len) == 0 && name[len] == '/';
}

static void gen_dir(pull_state *p, size_t ndx, const char *local)
{
    const ars_file *f = &p->fl.files[ndx];
    const char *name = flist_name(&p->fl, f);
    plat_fileinfo st;
    uint16_t iflags = 0;
    int exists, rc;

    rc = plat_stat(local, &st);
    exists = rc == PLAT_OK && st.type == PLAT_T_DIR;

    if (rc == PLAT_OK && !exists && !p->o->dry_run) {
        /* A file is in the way: replace it, as rsync does. */
        if ((rc = plat_remove(local)) != PLAT_OK) {
            pull_file_error(p, "arsync: cannot replace %s with a directory: %s",
                            local, plat_strerror(rc));
            goto skip_contents;
        }
    }

    if (!exists || (ndx == 0 && p->dest_created && strcmp(name, ".") == 0)) {
        iflags = ITEM_LOCAL_CHANGE | ITEM_IS_NEW;
        if (!exists && !p->o->dry_run && (rc = plat_mkdir(local)) != PLAT_OK
            && rc != PLAT_EEXIST) {
            pull_file_error(p, "arsync: recv_generator: mkdir %s failed: %s",
                            local, plat_strerror(rc));
            goto skip_contents;
        }
    } else if (p->o->times && st.mtime != f->mtime) {
        iflags = ITEM_REPORT_TIME;
    }

    if ((iflags & ITEM_SIGNIFICANT) || p->o->verbose > 1)
        write_item(p, (int32_t)ndx, iflags);
    return;

skip_contents:
    fprintf(stderr, "*** Skipping any contents from this failed directory ***\n");
    snprintf(p->failed_dir, sizeof p->failed_dir, "%s", name);
}

static void gen_file(pull_state *p, size_t ndx, const char *local)
{
    const ars_file *f = &p->fl.files[ndx];
    plat_fileinfo st;
    uint16_t iflags = ITEM_TRANSFER;
    int rc;

    rc = plat_stat(local, &st);
    if (rc == PLAT_OK && st.type == PLAT_T_DIR) {
        /* Like rsync without --force: an empty directory makes way. */
        if (p->o->dry_run) {
            rc = PLAT_ENOENT;
        } else if ((rc = plat_remove(local)) != PLAT_OK) {
            printf("cannot delete non-empty directory: %s\n", flist_name(&p->fl, f));
            pull_file_error(p, "arsync: could not make way for new regular file: %s", local);
            return;
        } else {
            rc = PLAT_ENOENT;
        }
    }
    if (rc != PLAT_OK && rc != PLAT_ENOENT) {
        pull_file_error(p, "arsync: recv_generator: failed to stat %s: %s",
                        local, plat_strerror(rc));
        return;
    }

    if (rc == PLAT_OK) {
        /* rsync's quick check: same size and same modification second. */
        if (st.size == f->size && st.mtime == f->mtime) {
            if (p->o->verbose > 1)
                write_item(p, (int32_t)ndx, 0);
            return;
        }
        if (st.size != f->size)
            iflags |= ITEM_REPORT_SIZE;
        if (!p->o->times || st.mtime != f->mtime)
            iflags |= ITEM_REPORT_TIME;
    } else {
        iflags |= ITEM_IS_NEW;
    }

    write_item(p, (int32_t)ndx, iflags);
    if (!p->o->dry_run)
        write_empty_sum_head(p);
}

int gen_entry(pull_state *p, size_t ndx)
{
    const ars_file *f = &p->fl.files[ndx];
    char local[FLIST_MAXPATH];

    if (!f->mode)
        return RERR_OK;
    if (p->failed_dir[0] && is_below(p->failed_dir, flist_name(&p->fl, f))) {
        p->fstate[ndx] |= FS_SKIPPED;
        return RERR_OK;
    }
    p->failed_dir[0] = '\0';

    pull_local_path(p, ndx, local, sizeof local);
    if (WIRE_ISDIR(f->mode))
        gen_dir(p, ndx, local);
    else if (WIRE_ISREG(f->mode))
        gen_file(p, ndx, local);
    else
        /* Symlinks and the like are listed even though we don't ask for
         * them; rsync always says it skips them. */
        printf("skipping non-regular file \"%s\"\n", flist_name(&p->fl, f));

    return p->s->out.err ? session_stream_failure(p->s, "sending requests") : RERR_OK;
}

void gen_redo(pull_state *p, int32_t ndx)
{
    write_item(p, ndx, ITEM_TRANSFER);
    write_empty_sum_head(p);
}
