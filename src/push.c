/* push.c - the sending side: local files to an rsync daemon
 *
 * We build and send the file list, then answer the server. Its generator
 * asks for files (with checksums of the copies it has); we reply in order
 * with each file as a delta. Items that changed without a transfer (a new
 * directory, say) are echoed back, so both sides log them.
 *
 * The exchange after the file list (S = server writes, W = we write):
 *
 *   S: requests ... NDX_DONE         phase 1
 *   W: replies  ... NDX_DONE
 *   S: redo requests ... NDX_DONE    phase 2: files that failed verification
 *   W: redo replies  ... NDX_DONE
 *   S: NDX_DONE                      no delay-updates phase
 *   W: NDX_DONE                      we're done sending
 *   S: NDX_DONE                      (protocol >= 31) goodbye
 *   W: NDX_DONE                      (protocol >= 31) its echo
 *   S: NDX_DONE                      final goodbye
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "push.h"
#include "match.h"
#include "output.h"
#include "progress.h"
#include "pull.h"
#include "scan.h"

typedef struct push_state {
    ars_session *s;
    const ars_options *o;
    ars_flist fl;
    char base[FLIST_MAXPATH];
    ars_ndx_state in_ndx, out_ndx;
    int io_error;
    long xfr_count;
    ars_progress progress;
    ars_match_ctx match;
    ars_token_tx tx;
} push_state;

/* With -z, files that are already compressed are sent at level 0, as
 * rsync's --skip-compress default does (plus Amiga archive types). */
static int skip_compress(const char *name)
{
    static const char *const suffixes[] = {
        "7z", "avi", "bz2", "deb", "dms", "flac", "gif", "gz", "jpeg", "jpg", "lha",
        "lz", "lz4", "lzh", "lzma", "lzo", "lzx", "mkv", "mov", "mp3", "mp4", "mpeg",
        "mpg", "ogg", "png", "rar", "rpm", "tbz", "tgz", "txz", "webm", "webp", "xz",
        "z", "zip", "zst",
    };
    const char *dot = strrchr(name, '.'), *suf;
    size_t i, j;

    if (!dot || strchr(dot, '/'))
        return 0;
    for (i = 0; i < sizeof suffixes / sizeof suffixes[0]; i++) {
        suf = suffixes[i];
        for (j = 0; suf[j] && dot[1 + j] && (dot[1 + j] | 0x20) == suf[j]; j++)
            ;
        if (!suf[j] && !dot[1 + j])
            return 1;
    }
    return 0;
}

static int flush(push_state *p)
{
    if (mux_flush(&p->s->mux) != WIRE_OK || conn_flush(&p->s->conn) != WIRE_OK) {
        if (!p->s->out.err)
            p->s->out.err = p->s->conn.w.err ? p->s->conn.w.err : WIRE_EIO;
        return -1;
    }
    return 0;
}

static void log_name(push_state *p, const ars_file *f)
{
    if (p->o->verbose || p->o->progress)
        printf("%s%s\n", flist_name(&p->fl, f), WIRE_ISDIR(f->mode) ? "/" : "");
}

static void show_progress(void *ctx, int64_t done)
{
    progress_update(&((push_state *)ctx)->progress, done);
}

static void echo_item(push_state *p, int32_t ndx, uint16_t iflags, int basis_type,
                      const char *xname, int xlen)
{
    w_ndx(&p->s->out, &p->out_ndx, ndx);
    w_uint16(&p->s->out, iflags);
    if (iflags & ITEM_BASIS_TYPE_FOLLOWS)
        w_byte(&p->s->out, (uint8_t)basis_type);
    if (iflags & ITEM_XNAME_FOLLOWS)
        w_vstring(&p->s->out, xname, (size_t)xlen);
}

static void send_no_send(push_state *p, int32_t ndx)
{
    uint8_t b[4];

    b[0] = (uint8_t)ndx;
    b[1] = (uint8_t)((uint32_t)ndx >> 8);
    b[2] = (uint8_t)((uint32_t)ndx >> 16);
    b[3] = (uint8_t)((uint32_t)ndx >> 24);
    mux_send_msg(&p->s->mux, MSG_NO_SEND, b, 4);
}

/* Handles one request. Sets *done on NDX_DONE. */
static int handle_request(push_state *p, int *done)
{
    ars_reader *in = &p->s->in;
    char xname[FLIST_MAXPATH], local[FLIST_MAXPATH];
    int basis_type = 0, xlen = 0, rc;
    const ars_file *f;
    plat_file *fh;
    ars_sums sums;
    uint16_t iflags;
    int32_t ndx;

    *done = 0;
    ndx = r_ndx_skip_stats(in, &p->in_ndx);
    if (in->err)
        return session_stream_failure(p->s, "reading request");
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
    if (iflags & ITEM_BASIS_TYPE_FOLLOWS)
        basis_type = r_byte(in);
    if (iflags & ITEM_XNAME_FOLLOWS)
        xlen = r_vstring(in, xname, sizeof xname);
    if (in->err)
        return session_stream_failure(p->s, "reading request");

    if (!(iflags & ITEM_TRANSFER) || p->o->dry_run) {
        /* Report changes as rsync does: directories that changed, and in
         * a dry run whatever would be sent. */
        if ((iflags & ITEM_TRANSFER)
            || ((iflags & ITEM_SIGNIFICANT) && (WIRE_ISDIR(f->mode) || (iflags & ITEM_LOCAL_CHANGE))))
            log_name(p, f);
        echo_item(p, ndx, iflags, basis_type, xname, xlen);
        return RERR_OK;
    }
    if (!WIRE_ISREG(f->mode)) {
        fprintf(stderr, "arsync: protocol error: transfer of non-regular file %s\n", flist_name(&p->fl, f));
        return RERR_PROTOCOL;
    }

    if ((rc = sums_read(&sums, in)) != WIRE_OK) {
        if (rc == WIRE_EPROTO) {
            fprintf(stderr, "arsync: protocol error: invalid checksums for %s\n", flist_name(&p->fl, f));
            return RERR_PROTOCOL;
        }
        if (rc == WIRE_ENOMEM) {
            fprintf(stderr, "arsync: out of memory reading checksums\n");
            return RERR_MALLOC;
        }
        return session_stream_failure(p->s, "reading checksums");
    }

    scan_local_path(&p->fl, f, p->base, local, sizeof local);
    if ((rc = plat_open_read(local, &fh)) != PLAT_OK) {
        fflush(stdout);
        if (rc == PLAT_ENOENT) {
            fprintf(stderr, "file has vanished: \"%s\"\n", local);
            p->io_error |= IOERR_VANISHED;
        } else {
            fprintf(stderr, "arsync: send_files failed to open \"%s\": %s\n", local, plat_strerror(rc));
            p->io_error |= IOERR_GENERAL;
        }
        sums_free(&sums);
        send_no_send(p, ndx);
        return RERR_OK;
    }

    echo_item(p, ndx, iflags, basis_type, xname, xlen);
    w_int32(&p->s->out, sums.h.count);
    w_int32(&p->s->out, sums.h.blength);
    w_int32(&p->s->out, sums.h.s2length);
    w_int32(&p->s->out, sums.h.remainder);

    if (p->o->progress) {
        log_name(p, f);
        progress_start(&p->progress, f->size);
    }
    p->match.read_error = 0;
    token_tx_level(&p->tx, skip_compress(flist_name(&p->fl, f)) ? 0 : p->o->compress_level);
    rc = send_delta(&p->match, fh, &sums, &p->s->out);
    plat_close(fh);
    sums_free(&sums);
    if (rc == WIRE_EABORT) {
        fflush(stdout);
        fprintf(stderr, "\narsync: interrupted by user\n");
        return RERR_SIGNAL;
    }
    if (rc == WIRE_EPROTO) {
        fprintf(stderr, "arsync: compression failed sending \"%s\"\n", local);
        return RERR_STREAMIO;
    }
    if (rc != WIRE_OK)
        return session_stream_failure(p->s, "sending file data");
    if (p->match.read_error) {
        fflush(stdout);
        fprintf(stderr, "arsync: read errors mapping \"%s\"\n", local);
        p->io_error |= IOERR_GENERAL;
    }
    p->xfr_count++;
    if (p->o->progress)
        progress_end(&p->progress, f->size, p->xfr_count, (long)(p->fl.count - ndx - 1),
                     (long)p->fl.count);
    else
        log_name(p, f);
    return RERR_OK;
}

static int expect_done(push_state *p, const char *when)
{
    int32_t ndx = r_ndx_skip_stats(&p->s->in, &p->in_ndx);

    if (p->s->in.err)
        return session_stream_failure(p->s, when);
    if (ndx != NDX_DONE) {
        fprintf(stderr, "arsync: protocol error: unexpected index %ld %s\n", (long)ndx, when);
        return RERR_PROTOCOL;
    }
    return RERR_OK;
}

static int run_sender(push_state *p)
{
    uint8_t b[4];
    int phase = 0, done, rc;

    for (;;) {
        if ((rc = handle_request(p, &done)) != RERR_OK)
            return rc;
        if (!done)
            continue;
        if (++phase > 2)
            break;
        w_ndx(&p->s->out, &p->out_ndx, NDX_DONE);
        if (flush(p) < 0)
            return session_stream_failure(p->s, "ending a phase");
    }

    if (p->io_error) {
        b[0] = (uint8_t)p->io_error;
        b[1] = b[2] = b[3] = 0;
        mux_send_msg(&p->s->mux, MSG_IO_ERROR, b, 4);
    }
    w_ndx(&p->s->out, &p->out_ndx, NDX_DONE);
    if (flush(p) < 0)
        return session_stream_failure(p->s, "finishing");

    if ((rc = expect_done(p, "in goodbye")) != RERR_OK)
        return rc;
    if (p->s->protocol >= 31) {
        w_ndx(&p->s->out, &p->out_ndx, NDX_DONE);
        if (flush(p) < 0)
            return session_stream_failure(p->s, "in goodbye");
        if ((rc = expect_done(p, "in final goodbye")) != RERR_OK)
            return rc;
    }
    return RERR_OK;
}

static void print_summary(push_state *p, int64_t total_size)
{
    char sent[32], recvd[32], rate[40], size[32];
    int64_t out = p->s->conn.bytes_out, in = p->s->conn.bytes_in, total = out + in;
    long elapsed = (long)(time(NULL) - p->s->started);
    int64_t speedup;

    comma_num_frac2((total * 200 + elapsed) / (1 + 2 * (int64_t)elapsed), rate);
    printf("\nsent %s bytes  received %s bytes  %s bytes/sec\n",
           comma_num(out, sent), comma_num(in, recvd), rate);
    speedup = total ? (total_size * 100 + total / 2) / total : 0;
    printf("total size is %s  speedup is %s%s\n", comma_num(total_size, size),
           comma_num_frac2(speedup, rate), p->o->dry_run ? " (DRY RUN)" : "");
}

int client_push(ars_session *s, const ars_options *o, const char *src)
{
    ars_flist_format fmt;
    push_state p;
    int64_t total_size = 0;
    size_t i;
    int rc, io_error, show_flist;

    memset(&p, 0, sizeof p);
    p.s = s;
    p.o = o;
    flist_init(&p.fl);
    ndx_state_init(&p.in_ndx);
    ndx_state_init(&p.out_ndx);
    /* An rsync receiver reads and writes independently, so our writes may
     * wait for it even while its requests pile up unread. */
    s->conn.block_writes = 1;
    /* rsync's byte counts start with the transfer, not the login. */
    s->conn.bytes_in = s->conn.bytes_out = 0;

    p.match.digest_type = s->checksum_type;
    p.match.seed = s->checksum_seed;
    p.match.seed_first = (s->compat_flags & CF_CHKSUM_SEED_FIX) != 0;
    p.match.progress = o->progress ? show_progress : NULL;
    p.match.progress_ctx = &p;
    p.match.tx = &p.tx;
    if (!(p.match.buf = malloc(MATCH_BUFSIZE)) || token_tx_init(&p.tx, s->compression) != WIRE_OK) {
        free(p.match.buf);
        token_tx_free(&p.tx);
        fprintf(stderr, "arsync: out of memory\n");
        return RERR_MALLOC;
    }

    show_flist = (o->verbose || o->progress) && o->recursive;
    if (show_flist)
        printf("building file list ... ");
    if (scan_source(&p.fl, src, o->recursive, &o->filters, p.base, sizeof p.base, &p.io_error) != WIRE_OK) {
        fprintf(stderr, "arsync: out of memory building file list\n");
        rc = RERR_MALLOC;
        goto out;
    }
    if (plat_break_pending()) {
        fprintf(stderr, "\narsync: interrupted by user\n");
        rc = RERR_SIGNAL;
        goto out;
    }
    flist_sort_and_clean(&p.fl);
    if (show_flist) {
        if (o->progress)
            printf("%lu file%sto consider\n", (unsigned long)p.fl.count, p.fl.count == 1 ? " " : "s ");
        else
            printf("done\n");
    }
    /* rsync's total counts everything but directories (a symlink's size
     * is its target's length). */
    for (i = 0; i < p.fl.count; i++)
        if (p.fl.files[i].mode && !WIRE_ISDIR(p.fl.files[i].mode))
            total_size += p.fl.files[i].size;

    fmt.varint_flags = (s->compat_flags & CF_VARINT_FLIST_FLAGS) != 0;
    fmt.safe_flist = (s->compat_flags & CF_SAFE_FLIST) || s->protocol >= 31;
    flist_send(&p.fl, &s->out, &fmt, s->protocol, p.io_error);
    if (flush(&p) < 0) {
        rc = session_stream_failure(s, "sending file list");
        goto out;
    }

    if ((rc = run_sender(&p)) != RERR_OK)
        goto out;
    if (o->verbose)
        print_summary(&p, total_size);

    io_error = p.io_error | s->remote_io_error | (s->saw_xfer_error ? IOERR_GENERAL : 0);
    rc = RERR_OK;
    if (io_error) {
        rc = io_error & IOERR_VANISHED && !(io_error & IOERR_GENERAL) ? RERR_VANISHED : RERR_PARTIAL;
        fflush(stdout);
        fprintf(stderr, "arsync error: %s (code %d)\n",
                rc == RERR_VANISHED ? "some files vanished before they could be transferred"
                                    : "some files/attrs were not transferred (see previous errors)",
                rc);
    }

out:
    free(p.match.buf);
    token_tx_free(&p.tx);
    flist_free(&p.fl);
    return rc;
}
