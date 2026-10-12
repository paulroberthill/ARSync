/* generator.c - decides what to fetch and writes the requests
 *
 * For each file-list entry: directories are created locally; regular files
 * that already match (same size and modification time, rsync's "quick
 * check") are skipped; anything else is requested, with checksums of the
 * existing copy's blocks if there is one, so that only changes are sent. Items that change
 * without a transfer (a new directory, say) are still reported to the
 * server, which echoes them back so the receiver can log them in order,
 * just as rsync does.
 */
#include <stdio.h>
#include <string.h>

#include "pull.h"
#include "checksum.h"
#include "plat.h"

static void write_item(pull_state *p, int32_t ndx, uint16_t iflags)
{
    w_ndx(&p->s->out, &p->gen_ndx, ndx);
    w_uint16(&p->s->out, iflags);
    p->outstanding++;
}

static void write_sum_head(pull_state *p, const ars_sum_head *sh)
{
    w_int32(&p->s->out, sh->count);
    w_int32(&p->s->out, sh->blength);
    w_int32(&p->s->out, sh->s2length);
    w_int32(&p->s->out, sh->remainder);
}

/* A checksum header with no blocks: the server sends the whole file. */
static void write_empty_sum_head(pull_state *p)
{
    static const ars_sum_head none = { 0, 0, 0, 0 };

    write_sum_head(p, &none);
}

/* Describes the existing file block by block, so the server can send only
 * what differs. Returns 0, or -1 if the file can't be opened (the caller
 * then asks for the whole file). */
static int write_block_sums(pull_state *p, const char *local, int64_t size, int min_s2length)
{
    uint8_t strong[SUM_LENGTH];
    ars_sum_head sh;
    plat_file *f;
    int32_t i, n;
    long got, r;

    if (sum_sizes(size, min_s2length, &sh) < 0 || plat_open_read(local, &f) != PLAT_OK)
        return -1;
    write_sum_head(p, &sh);
    for (i = 0; i < sh.count; i++) {
        n = i == sh.count - 1 && sh.remainder ? sh.remainder : sh.blength;
        /* A short read (the file shrank, or an I/O error) leaves zeros,
         * which just won't match; the header is already sent, so every
         * block must still be described. */
        for (got = 0; got < n; got += r) {
            r = plat_read(f, p->block + got, (size_t)(n - got));
            if (r <= 0)
                break;
        }
        if (got < n)
            memset(p->block + got, 0, (size_t)(n - got));
        block_sum(p->s->checksum_type, p->s->checksum_seed,
                  (p->s->compat_flags & CF_CHKSUM_SEED_FIX) != 0, p->block, (size_t)n, strong);
        w_int32(&p->s->out, (int32_t)rolling_sum(p->block, (size_t)n));
        w_bytes(&p->s->out, strong, (size_t)sh.s2length);
    }
    plat_close(f);
    return 0;
}

/* Asks for a file: against the existing copy if there's a usable one
 * (delta transfer), else whole. */
static void request_file(pull_state *p, int32_t ndx, uint16_t iflags, const char *local,
                         const plat_fileinfo *existing, int min_s2length)
{
    write_item(p, ndx, iflags);
    if (p->o->dry_run)
        return;
    if (existing && existing->size > 0 && !p->o->whole_file
        && write_block_sums(p, local, existing->size, min_s2length) == 0)
        return;
    write_empty_sum_head(p);
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

    request_file(p, (int32_t)ndx, iflags, local, rc == PLAT_OK ? &st : NULL, SHORT_SUM_LENGTH);
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

/* The redo uses full-length strong checksums, so a false block match
 * (the likely cause of the failure) can't happen again. */
void gen_redo(pull_state *p, int32_t ndx)
{
    char local[FLIST_MAXPATH];
    plat_fileinfo st;
    int exists;

    pull_local_path(p, (size_t)ndx, local, sizeof local);
    exists = plat_stat(local, &st) == PLAT_OK && st.type == PLAT_T_FILE;
    request_file(p, ndx, ITEM_TRANSFER | (exists ? 0 : ITEM_IS_NEW), local,
                 exists ? &st : NULL, SUM_LENGTH);
}
