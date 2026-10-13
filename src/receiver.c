/* receiver.c - reads the server's replies and writes the files
 *
 * Each reply starts with the file's index and item flags. Items without
 * ITEM_TRANSFER are only logged. A transfer continues with the checksum
 * header we sent, the file contents as a stream of tokens (positive: that
 * many literal bytes follow; negative: copy a block of the old file; zero:
 * end), and the whole-file checksum, which we verify before the temp file
 * replaces the real one.
 */
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "pull.h"
#include "checksum.h"
#include "digest.h"
#include "plat.h"

/* Longest name the temp file may have. AmigaDOS FFS allows 30 characters;
 * names are kept within that everywhere for simplicity. */
#define TMPNAME_MAX 30
#define TMPNAME_SUFFIX 7   /* ".XXXXXX" */

static void basis_close(pull_state *p);

static int stream_failure(pull_state *p, const char *what)
{
    return session_stream_failure(p->s, what);
}

/* rsync names its temp files ".NAME.XXXXXX" next to the target. */
static int make_tmpname(pull_state *p, const char *local)
{
    static unsigned long counter;
    const char *base = strrchr(local, '/');
    const char *colon = strrchr(local, ':');
    size_t dirlen, baselen;
    plat_fileinfo st;
    int tries;

    if (colon && (!base || colon > base))
        base = colon;
    base = base ? base + 1 : local;
    dirlen = (size_t)(base - local);
    if (*base == '.')
        base++;
    baselen = strlen(base);
    if (baselen > TMPNAME_MAX - 1 - TMPNAME_SUFFIX)
        baselen = TMPNAME_MAX - 1 - TMPNAME_SUFFIX;
    if (dirlen + 1 + baselen + TMPNAME_SUFFIX >= sizeof p->tmp)
        return -1;

    if (!counter)
        counter = (unsigned long)time(NULL);
    for (tries = 0; tries < 100; tries++) {
        snprintf(p->tmp, sizeof p->tmp, "%.*s.%.*s.%06lu", (int)dirlen, local,
                 (int)baselen, base, counter++ % 1000000);
        if (plat_stat(p->tmp, &st) == PLAT_ENOENT)
            return 0;
    }
    p->tmp[0] = '\0';
    return -1;
}

void recv_cleanup(pull_state *p)
{
    basis_close(p);
    if (p->tmp[0]) {
        plat_remove(p->tmp);
        p->tmp[0] = '\0';
    }
}

/* Prints a file's name as rsync -v does ("dir/" for directories). */
static void log_name(pull_state *p, size_t ndx)
{
    const ars_file *f = &p->fl.files[ndx];
    const char *name = flist_name(&p->fl, f);

    if (!p->o->verbose && !p->o->progress)
        return;
    printf("%s%s\n", name, WIRE_ISDIR(f->mode) ? "/" : "");
}

static void basis_close(pull_state *p)
{
    if (p->basis) {
        plat_close(p->basis);
        p->basis = NULL;
    }
}

/* Returns a pointer to len bytes of the old file at off, or NULL if they
 * can't be read. Reads through a window, since a delta mostly refers to
 * blocks in order. */
static const uint8_t *basis_get(pull_state *p, int64_t off, size_t len)
{
    long r;

    if (!p->basis || off < 0 || off + (int64_t)len > p->basis_size)
        return NULL;
    if (off < p->basis_off || off + (int64_t)len > p->basis_off + (int64_t)p->basis_len) {
        if (plat_seek(p->basis, off) != PLAT_OK)
            return NULL;
        p->basis_off = off;
        p->basis_len = 0;
        while (p->basis_len < BASIS_CACHE) {
            r = plat_read(p->basis, p->basis_buf + p->basis_len, BASIS_CACHE - p->basis_len);
            if (r <= 0)
                break;
            p->basis_len += (size_t)r;
        }
        if ((int64_t)p->basis_len < (int64_t)len)
            return NULL;
    }
    return p->basis_buf + (off - p->basis_off);
}

/* Reads the token stream into f (or nowhere, if f is NULL), feeding the
 * checksum: literal data comes from the server, matched blocks from the
 * old file. Returns RERR_OK, a fatal code, or -1 on a write error. A block
 * that can't be read from the old file is simply missing from the result,
 * which then fails verification and is fetched again. */
static int read_tokens(pull_state *p, const ars_sum_head *sh, plat_file *f,
                       ars_digest *sum, int64_t *written)
{
    ars_reader *in = &p->s->in;
    const uint8_t *data;
    int32_t n, len;
    int write_failed = 0, err, rc;

    *written = 0;
    for (;;) {
        if ((rc = pull_check_break(p)) != RERR_OK)
            return rc;
        if (p->o->progress)
            progress_update(&p->progress, *written);
        n = token_recv(&p->tok, in, &data, &err);
        if (err == WIRE_EPROTO && !in->err) {
            fprintf(stderr, "arsync: error in compressed file data\n");
            return RERR_STREAMIO;
        }
        if (err == WIRE_ENOMEM) {
            fprintf(stderr, "arsync: out of memory\n");
            return RERR_MALLOC;
        }
        if (err)
            return stream_failure(p, "receiving file data");
        if (n == 0)
            break;
        if (n < 0) {
            n = -(n + 1);
            if (n >= sh->count) {
                fprintf(stderr, "arsync: protocol error: block reference %ld out of range\n", (long)n);
                return RERR_PROTOCOL;
            }
            len = n == sh->count - 1 && sh->remainder ? sh->remainder : sh->blength;
            data = basis_get(p, (int64_t)n * sh->blength, (size_t)len);
            if (data) {
                if (token_see(&p->tok, data, len) != WIRE_OK) {
                    fprintf(stderr, "arsync: error in compressed file data\n");
                    return RERR_STREAMIO;
                }
                digest_update(sum, data, (size_t)len);
                if (f && !write_failed && plat_write(f, data, (size_t)len) != PLAT_OK)
                    write_failed = 1;
            }
            *written += len;
            continue;
        }
        digest_update(sum, data, (size_t)n);
        if (f && !write_failed && plat_write(f, data, (size_t)n) != PLAT_OK)
            write_failed = 1;
        *written += n;
    }
    return write_failed ? -1 : RERR_OK;
}

static int read_sum_head(pull_state *p, ars_sum_head *sh)
{
    ars_reader *in = &p->s->in;

    sh->count = r_int32(in);
    sh->blength = r_int32(in);
    sh->s2length = r_int32(in);
    sh->remainder = r_int32(in);
    if (in->err)
        return stream_failure(p, "reading checksum header");
    if (sh->count < 0 || sh->blength < 0 || sh->blength > MAX_BLOCK_SIZE || sh->s2length < 0
        || sh->s2length > DIGEST_LEN || sh->remainder < 0 || sh->remainder > sh->blength
        || (sh->count > 0 && sh->blength == 0)) {
        fprintf(stderr, "arsync: protocol error: invalid checksum header\n");
        return RERR_PROTOCOL;
    }
    return RERR_OK;
}

static int receive_file(pull_state *p, size_t ndx)
{
    const ars_file *fe = &p->fl.files[ndx];
    char local[FLIST_MAXPATH];
    uint8_t want[DIGEST_LEN], got[DIGEST_LEN];
    plat_fileinfo st;
    plat_file *f = NULL;
    ars_sum_head sh;
    ars_digest sum;
    int64_t written;
    int rc, existed;

    pull_local_path(p, ndx, local, sizeof local);
    if ((rc = read_sum_head(p, &sh)) != RERR_OK)
        return rc;

    /* The checksums we sent describe the file now at the destination;
     * that's the old file the server's block references point into. */
    basis_close(p);
    p->basis_len = 0;
    if (sh.count > 0 && plat_stat(local, &st) == PLAT_OK
        && plat_open_read(local, &p->basis) == PLAT_OK)
        p->basis_size = st.size;

    if (make_tmpname(p, local) < 0)
        pull_file_error(p, "arsync: cannot make a temporary name for %s", local);
    else if ((rc = plat_open_write(p->tmp, &f)) != PLAT_OK) {
        pull_file_error(p, "arsync: cannot create %s: %s", p->tmp, plat_strerror(rc));
        p->tmp[0] = '\0';
    }

    /* With --progress the name comes first, then the progress line. */
    if (p->o->progress) {
        printf("%s\n", flist_name(&p->fl, fe));
        progress_start(&p->progress, fe->size);
    }

    /* Even without somewhere to write it, the data has to be read. */
    digest_init(&sum, p->s->checksum_type);
    rc = read_tokens(p, &sh, f, &sum, &written);
    basis_close(p);
    if (rc > 0) {
        if (f)
            plat_close(f);
        return rc;
    }
    r_bytes(&p->s->in, want, DIGEST_LEN);
    if (p->s->in.err) {
        if (f)
            plat_close(f);
        return stream_failure(p, "reading file checksum");
    }
    digest_final(&sum, got);
    p->xfr_count++;
    if (p->o->progress)
        progress_end(&p->progress, written, p->xfr_count,
                     (long)(p->fl.count - ndx - 1), (long)p->fl.count);
    if (!f)
        return RERR_OK;

    if (plat_close(f) != PLAT_OK || rc < 0) {
        /* rsync stops the whole run on a write error (disk full, say). */
        fprintf(stderr, "arsync: write failed on %s\n", local);
        recv_cleanup(p);
        return RERR_FILEIO;
    }

    if (memcmp(want, got, DIGEST_LEN) != 0) {
        recv_cleanup(p);
        if (!p->redoing && !(p->fstate[ndx] & FS_REDO)) {
            if (p->o->verbose)
                fprintf(stderr, "WARNING: %s failed verification -- update discarded (will try again).\n",
                        flist_name(&p->fl, fe));
            p->fstate[ndx] |= FS_REDO;
            p->redo[p->redo_count++] = (int32_t)ndx;
        } else {
            pull_file_error(p, "ERROR: %s failed verification -- update discarded.",
                            flist_name(&p->fl, fe));
        }
        return RERR_OK;
    }

    existed = plat_stat(local, &st) == PLAT_OK && st.type == PLAT_T_FILE;
    plat_new_file_perms(p->tmp, fe->mode, existed ? local : NULL);
    if (p->o->times && (rc = plat_set_mtime(p->tmp, plat_clamp_mtime(fe->mtime),
                                            (long)fe->mtime_nsec)) != PLAT_OK)
        pull_file_error(p, "arsync: failed to set times on %s: %s", local, plat_strerror(rc));
    if ((rc = plat_rename(p->tmp, local)) != PLAT_OK) {
        pull_file_error(p, "arsync: rename %s -> %s: %s", p->tmp, local, plat_strerror(rc));
        recv_cleanup(p);
        return RERR_OK;
    }
    p->tmp[0] = '\0';
    if (!p->o->progress)
        log_name(p, ndx);
    return RERR_OK;
}

int recv_reply(pull_state *p, int until_done, int *done)
{
    ars_reader *in = &p->s->in;
    int32_t ndx;
    uint16_t iflags;
    const ars_file *f;
    int rc;

    *done = 0;
    for (;;) {
        /* Files the server couldn't open (it has said why) get no reply. */
        if (p->s->no_send_pending) {
            p->outstanding -= p->s->no_send_pending < p->outstanding
                              ? p->s->no_send_pending : p->outstanding;
            p->s->no_send_pending = 0;
            p->xfer_error = 1;
        }
        if (!until_done && p->outstanding == 0)
            return RERR_OK;
        rc = session_wait_reply(p->s);
        if (rc == WIRE_OK)
            break;
        if (rc != WIRE_EINTR)
            return stream_failure(p, "reading reply");
    }

    ndx = r_ndx_skip_stats(in, &p->recv_ndx);
    if (in->err)
        return stream_failure(p, "reading reply");
    if (ndx == NDX_DONE) {
        *done = 1;
        return RERR_OK;
    }
    if (ndx < 0 || (size_t)ndx >= p->fl.count || !p->fl.files[ndx].mode) {
        fprintf(stderr, "arsync: protocol error: invalid file index %ld\n", (long)ndx);
        return RERR_PROTOCOL;
    }
    f = &p->fl.files[ndx];

    iflags = r_uint16(in);
    if (in->err)
        return stream_failure(p, "reading reply");
    if (iflags & (ITEM_BASIS_TYPE_FOLLOWS | ITEM_XNAME_FOLLOWS)) {
        fprintf(stderr, "arsync: protocol error: unexpected basis file in reply\n");
        return RERR_PROTOCOL;
    }
    if ((iflags & ITEM_TRANSFER) && !WIRE_ISREG(f->mode)) {
        fprintf(stderr, "arsync: protocol error: transfer of non-regular file %s\n",
                flist_name(&p->fl, f));
        return RERR_PROTOCOL;
    }
    if (p->outstanding > 0)
        p->outstanding--;

    if (!(iflags & ITEM_TRANSFER)) {
        /* Like rsync: report directories that changed (created, new time);
         * with -vv also say which files were already up to date. */
        if ((iflags & ITEM_SIGNIFICANT) && (WIRE_ISDIR(f->mode) || (iflags & ITEM_LOCAL_CHANGE)))
            log_name(p, (size_t)ndx);
        else if (!(iflags & ITEM_SIGNIFICANT) && p->o->verbose > 1)
            printf("%s is uptodate\n", flist_name(&p->fl, f));
        return RERR_OK;
    }
    if (p->o->dry_run) {
        log_name(p, (size_t)ndx);
        return RERR_OK;
    }
    return receive_file(p, (size_t)ndx);
}
