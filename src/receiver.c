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
#include "digest.h"
#include "plat.h"

/* Longest name the temp file may have. AmigaDOS FFS allows 30 characters;
 * names are kept within that everywhere for simplicity. */
#define TMPNAME_MAX 30
#define TMPNAME_SUFFIX 7   /* ".XXXXXX" */

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

    if (!p->o->verbose)
        return;
    printf("%s%s\n", name, WIRE_ISDIR(f->mode) ? "/" : "");
}

/* Reads the token stream into f (or nowhere, if f is NULL), feeding the
 * checksum. Returns RERR_OK, a fatal code, or -1 on a write error. */
static int read_tokens(pull_state *p, plat_file *f, ars_digest *sum, int64_t *written)
{
    ars_reader *in = &p->s->in;
    int32_t n;
    size_t chunk;
    int write_failed = 0;

    *written = 0;
    for (;;) {
        n = r_int32(in);
        if (in->err)
            return stream_failure(p, "receiving file data");
        if (n == 0)
            break;
        if (n < 0) {
            /* We never send block checksums, so the server has no blocks
             * of ours to refer to. */
            fprintf(stderr, "arsync: protocol error: unexpected block reference\n");
            return RERR_PROTOCOL;
        }
        while (n > 0) {
            chunk = n > CHUNK_SIZE ? CHUNK_SIZE : (size_t)n;
            r_bytes(in, p->buf, chunk);
            if (in->err)
                return stream_failure(p, "receiving file data");
            digest_update(sum, p->buf, chunk);
            if (f && !write_failed && plat_write(f, p->buf, chunk) != PLAT_OK)
                write_failed = 1;
            *written += (int64_t)chunk;
            n -= (int32_t)chunk;
        }
    }
    return write_failed ? -1 : RERR_OK;
}

static int read_sum_head(pull_state *p)
{
    ars_reader *in = &p->s->in;
    int32_t count = r_int32(in), blength = r_int32(in);
    int32_t s2length = r_int32(in), remainder = r_int32(in);

    if (in->err)
        return stream_failure(p, "reading checksum header");
    if (count < 0 || blength < 0 || blength > MAX_BLOCK_SIZE || s2length < 0
        || s2length > DIGEST_LEN || remainder < 0 || remainder > blength) {
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
    ars_digest sum;
    int64_t written;
    int rc, existed;

    pull_local_path(p, ndx, local, sizeof local);
    if ((rc = read_sum_head(p)) != RERR_OK)
        return rc;

    if (make_tmpname(p, local) < 0)
        pull_file_error(p, "arsync: cannot make a temporary name for %s", local);
    else if ((rc = plat_open_write(p->tmp, &f)) != PLAT_OK) {
        pull_file_error(p, "arsync: cannot create %s: %s", p->tmp, plat_strerror(rc));
        p->tmp[0] = '\0';
    }

    /* Even without somewhere to write it, the data has to be read. */
    digest_init(&sum, p->s->checksum_type);
    rc = read_tokens(p, f, &sum, &written);
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
    if (p->o->times && (rc = plat_set_mtime(p->tmp, fe->mtime, (long)fe->mtime_nsec)) != PLAT_OK)
        pull_file_error(p, "arsync: failed to set times on %s: %s", local, plat_strerror(rc));
    if ((rc = plat_rename(p->tmp, local)) != PLAT_OK) {
        pull_file_error(p, "arsync: rename %s -> %s: %s", p->tmp, local, plat_strerror(rc));
        recv_cleanup(p);
        return RERR_OK;
    }
    p->tmp[0] = '\0';
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

    ndx = r_ndx(in, &p->recv_ndx);
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
