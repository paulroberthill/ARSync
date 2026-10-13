/* generator.c - decides what to fetch and writes the requests
 *
 * For each file-list entry: directories are created locally; regular files
 * that already match (same size and modification time, rsync's "quick
 * check"; or size alone with --size-only, size and whole-file checksum with
 * -c) are skipped, as are those -u, --existing and --ignore-existing rule
 * out; anything else is requested, with checksums of the existing copy's
 * blocks if there is one, so that only changes are sent. Items that change
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
        /* Checksumming a big file takes a while on a 68k. Once Ctrl-C is
         * pressed the run ends, so the remaining blocks don't matter. */
        if ((i & 15) == 0 && plat_break_pending())
            break;
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

    if (rc == PLAT_ENOENT && p->o->existing) {
        /* Neither it nor anything in it gets created. */
        if (p->o->verbose > 1)
            printf("not creating new directory \"%s\"\n", name);
        snprintf(p->failed_dir, sizeof p->failed_dir, "%s", name);
        p->fstate[ndx] |= FS_SKIPPED;
        return;
    }
    if (rc == PLAT_OK && !exists && p->o->ignore_existing) {
        if (p->o->verbose > 1)
            printf("%s exists\n", name);
        snprintf(p->failed_dir, sizeof p->failed_dir, "%s", name);
        p->fstate[ndx] |= FS_SKIPPED;
        return;
    }

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
    } else if (p->o->times && st.mtime != plat_clamp_mtime(f->mtime)) {
        iflags = ITEM_REPORT_TIME;
    }

    if ((iflags & ITEM_SIGNIFICANT) || p->o->verbose > 1 || p->o->itemize > 1)
        write_item(p, (int32_t)ndx, iflags);
    /* A directory that was already there may hold things to delete. */
    if (exists && p->o->delete_mode && !(f->xflags & XMIT_NO_CONTENT_DIR))
        delete_in_dir(p, ndx, local);
    return;

skip_contents:
    fprintf(stderr, "*** Skipping any contents from this failed directory ***\n");
    snprintf(p->failed_dir, sizeof p->failed_dir, "%s", name);
}

/* rsync's quick check: is the existing file already up to date? Same size
 * and then, by default, the same modification second (as the local
 * filesystem can record it); with --size-only nothing more; with -c the
 * same whole-file checksum instead of the time. */
static int quick_check_ok(pull_state *p, const ars_file *f, const char *local,
                          const plat_fileinfo *st)
{
    uint8_t sum[SUM_LENGTH];

    if (st->size != f->size)
        return 0;
    if (p->o->checksum) {
        file_sum(local, p->s->checksum_type, p->block, MAX_BLOCK_SIZE, sum);
        return memcmp(sum, flist_sum(&p->fl, f), (size_t)p->fl.sum_len) == 0;
    }
    if (p->o->size_only)
        return 1;
    return st->mtime == plat_clamp_mtime(f->mtime);
}

static void gen_file(pull_state *p, size_t ndx, const char *local)
{
    const ars_file *f = &p->fl.files[ndx];
    const char *name = flist_name(&p->fl, f);
    plat_fileinfo st;
    uint16_t iflags = ITEM_TRANSFER;
    int rc;

    rc = plat_stat(local, &st);

    /* What -u, --existing and --ignore-existing rule out, in rsync's
     * order (before anything in the way is replaced). */
    if (rc == PLAT_ENOENT && p->o->existing) {
        if (p->o->verbose > 1)
            printf("not creating new file \"%s\"\n", name);
        return;
    }
    if (rc == PLAT_OK && p->o->ignore_existing) {
        if (p->o->verbose > 1)
            printf("%s exists\n", name);
        return;
    }
    if (rc == PLAT_OK && p->o->update && plat_clamp_mtime(f->mtime) < st.mtime) {
        if (p->o->verbose > 1)
            printf("%s is newer\n", name);
        return;
    }

    if (rc == PLAT_OK && st.type == PLAT_T_DIR) {
        /* Like rsync without --force: an empty directory makes way. */
        if (p->o->dry_run) {
            rc = PLAT_ENOENT;
        } else if (p->o->delete_mode && !p->dest_is_file) {
            /* With --delete, contents and all. */
            if (delete_for_file(p, ndx, local) != PLAT_OK) {
                pull_file_error(p, "arsync: could not make way for new regular file: %s", local);
                return;
            }
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
        if (quick_check_ok(p, f, local, &st)) {
            /* Up to date, though with --size-only or -c the time may
             * still need setting (and is then reported). */
            if (p->o->times && st.mtime != plat_clamp_mtime(f->mtime)) {
                if (!p->o->dry_run
                    && (rc = plat_set_mtime(local, plat_clamp_mtime(f->mtime),
                                            (long)f->mtime_nsec)) != PLAT_OK)
                    pull_file_error(p, "arsync: failed to set times on %s: %s",
                                    local, plat_strerror(rc));
                /* rsync's set_file_attrs() names it at -vv (without -i). */
                if (p->o->verbose > 1 && !p->o->itemize)
                    printf("%s\n", name);
                write_item(p, (int32_t)ndx, ITEM_REPORT_TIME);
            } else if (p->o->verbose > 1 || p->o->itemize > 1) {
                write_item(p, (int32_t)ndx, 0);
            }
            return;
        }
        if (st.size != f->size)
            iflags |= ITEM_REPORT_SIZE;
        if (!p->o->times || st.mtime != plat_clamp_mtime(f->mtime))
            iflags |= ITEM_REPORT_TIME;
    } else {
        iflags |= ITEM_IS_NEW;
    }
    /* rsync marks every -c transfer as a checksum change. */
    if (p->o->checksum)
        iflags |= ITEM_REPORT_CHANGE;

    request_file(p, (int32_t)ndx, iflags, local, rc == PLAT_OK ? &st : NULL, SHORT_SUM_LENGTH);
}

/* Symlinks and the like are listed even though we don't ask for them;
 * rsync says it skips them, unless --existing or --ignore-existing (which
 * it checks first) rule them out anyway. */
static void gen_other(pull_state *p, size_t ndx, const char *local)
{
    const char *name = flist_name(&p->fl, &p->fl.files[ndx]);
    plat_fileinfo st;
    int rc;

    if (p->o->existing || p->o->ignore_existing) {
        rc = plat_stat(local, &st);
        if (rc == PLAT_ENOENT && p->o->existing) {
            if (p->o->verbose > 1)
                printf("not creating new file \"%s\"\n", name);
            return;
        }
        if (rc == PLAT_OK && p->o->ignore_existing) {
            if (p->o->verbose > 1)
                printf("%s exists\n", name);
            return;
        }
    }
    printf("skipping non-regular file \"%s\"\n", name);
}

int gen_entry(pull_state *p, size_t ndx)
{
    const ars_file *f = &p->fl.files[ndx];
    char local[FLIST_MAXPATH];
    const char *reason = NULL;

    if (!f->mode)
        return RERR_OK;
    if (p->failed_dir[0] && is_below(p->failed_dir, flist_name(&p->fl, f))) {
        p->fstate[ndx] |= FS_SKIPPED;
        return RERR_OK;
    }
    p->failed_dir[0] = '\0';

    /* Things the destination can't hold are skipped, never altered. */
    if (p->fstate[ndx] & FS_CASE_CLASH)
        reason = "same name as another file when case is ignored";
    else if (!p->dest_is_file)
        reason = fsmap_check_name(&p->rules, flist_name(&p->fl, f));
    if (!reason && WIRE_ISREG(f->mode) && f->size > p->rules.max_file_size)
        reason = "file too large for this filesystem";
    if (reason) {
        pull_file_error(p, "arsync: skipping \"%s\": %s", flist_name(&p->fl, f), reason);
        p->fstate[ndx] |= FS_SKIPPED;
        if (WIRE_ISDIR(f->mode))
            snprintf(p->failed_dir, sizeof p->failed_dir, "%s", flist_name(&p->fl, f));
        return RERR_OK;
    }

    pull_local_path(p, ndx, local, sizeof local);
    if (WIRE_ISDIR(f->mode))
        gen_dir(p, ndx, local);
    else if (WIRE_ISREG(f->mode))
        gen_file(p, ndx, local);
    else
        gen_other(p, ndx, local);

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
