/* client.c - the receiving side of a pull: file list, listing, transfer phases
 *
 * rsync runs the receiving side as two processes, a generator (decides
 * what to ask for, writes requests) and a receiver (reads what arrives).
 * Here both are one sequence of steps over the session's streams; the
 * buffered connection underneath keeps both directions moving.
 *
 * End-of-run exchange for a pull (G = our generator writes, R = our
 * receiver reads; S = server):
 *
 *   G: NDX_DONE                      end of phase 1 requests
 *   R: NDX_DONE                      S finished phase 1
 *   G: NDX_DONE                      end of phase 2 (redo) requests
 *   G: NDX_DONE                      no delay-updates phase
 *   G: NDX_DONE                      (protocol >= 31) no late-delete phase
 *   R: NDX_DONE, NDX_DONE            S finished phase 2, then all sending
 *   R: 5 x varlong                   S's statistics
 *   R: NDX_DONE                      (protocol >= 31) S's goodbye echo
 *   G: NDX_DONE                      our final goodbye
 */
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "client.h"
#include "flist.h"
#include "output.h"

/* The server's byte counts, already turned round to our point of view: it
 * sends what it read (= what we sent) first, then what it wrote. */
typedef struct server_stats {
    int64_t sent;
    int64_t received;
    int64_t total_size;
    int64_t flist_buildtime;
    int64_t flist_xfertime;
} server_stats;

typedef struct pull_state {
    ars_session *s;
    const ars_options *o;
    ars_ndx_state gen_ndx;   /* our outgoing indexes */
    ars_ndx_state recv_ndx;  /* the server's incoming indexes */
    ars_flist fl;
    server_stats stats;
} pull_state;

static int flush(pull_state *p)
{
    if (mux_flush(&p->s->mux) != WIRE_OK || conn_flush(&p->s->conn) != WIRE_OK) {
        if (!p->s->out.err)
            p->s->out.err = p->s->conn.w.err ? p->s->conn.w.err : WIRE_EIO;
        return -1;
    }
    return 0;
}

/* Reads one index from the server and insists it is NDX_DONE. */
static int expect_done(pull_state *p, const char *when)
{
    int32_t ndx = r_ndx(&p->s->in, &p->recv_ndx);

    if (p->s->in.err)
        return session_stream_failure(p->s, when);
    if (ndx != NDX_DONE) {
        fprintf(stderr, "arsync: protocol error: unexpected index %ld %s\n", (long)ndx, when);
        return RERR_PROTOCOL;
    }
    return RERR_OK;
}

static int finish_phases(pull_state *p)
{
    ars_reader *in = &p->s->in;
    ars_writer *out = &p->s->out;
    int rc;

    w_ndx(out, &p->gen_ndx, NDX_DONE);
    if (flush(p) < 0)
        return session_stream_failure(p->s, "ending phase 1");
    if ((rc = expect_done(p, "at end of phase 1")) != RERR_OK)
        return rc;

    w_ndx(out, &p->gen_ndx, NDX_DONE);
    w_ndx(out, &p->gen_ndx, NDX_DONE);
    if (p->s->protocol >= 31)
        w_ndx(out, &p->gen_ndx, NDX_DONE);
    if (flush(p) < 0)
        return session_stream_failure(p->s, "ending phase 2");
    if ((rc = expect_done(p, "at end of phase 2")) != RERR_OK
        || (rc = expect_done(p, "at end of transfer")) != RERR_OK)
        return rc;

    p->stats.sent = r_varlong(in, 3);
    p->stats.received = r_varlong(in, 3);
    p->stats.total_size = r_varlong(in, 3);
    p->stats.flist_buildtime = r_varlong(in, 3);
    p->stats.flist_xfertime = r_varlong(in, 3);
    if (in->err)
        return session_stream_failure(p->s, "reading statistics");

    if (p->s->protocol >= 31 && (rc = expect_done(p, "in goodbye")) != RERR_OK)
        return rc;

    w_ndx(out, &p->gen_ndx, NDX_DONE);
    if (flush(p) < 0)
        return session_stream_failure(p->s, "sending goodbye");
    return RERR_OK;
}

static void print_summary(pull_state *p)
{
    char sent[32], recvd[32], rate[40], size[32];
    int64_t total = p->stats.sent + p->stats.received;
    long elapsed = (long)(time(NULL) - p->s->started);
    int64_t speedup;

    /* rsync's rate is bytes / (0.5 + whole seconds), shown with 2 decimals:
     * in hundredths that's bytes * 200 / (1 + 2 * seconds), rounded. */
    comma_num_frac2((total * 200 + elapsed) / (1 + 2 * (int64_t)elapsed), rate);
    printf("\nsent %s bytes  received %s bytes  %s bytes/sec\n",
           comma_num(p->stats.sent, sent), comma_num(p->stats.received, recvd), rate);
    speedup = total ? (p->stats.total_size * 100 + total / 2) / total : 0;
    printf("total size is %s  speedup is %s\n",
           comma_num(p->stats.total_size, size), comma_num_frac2(speedup, rate));
}

static int list_files(pull_state *p)
{
    char line[FLIST_MAXPATH + 64];
    size_t i;

    for (i = 0; i < p->fl.count; i++) {
        if (!p->fl.files[i].mode)
            continue;
        format_list_entry(&p->fl, &p->fl.files[i], line, sizeof line);
        printf("%s\n", line);
    }
    return RERR_OK;
}

int client_pull(ars_session *s, const ars_options *o, const char *dest)
{
    ars_flist_format fmt;
    pull_state p;
    char err[FLIST_MAXPATH + 64];
    int rc, io_error;

    memset(&p, 0, sizeof p);
    p.s = s;
    p.o = o;
    ndx_state_init(&p.gen_ndx);
    ndx_state_init(&p.recv_ndx);
    flist_init(&p.fl);

    fmt.varint_flags = (s->compat_flags & CF_VARINT_FLIST_FLAGS) != 0;
    fmt.safe_flist = (s->compat_flags & CF_SAFE_FLIST) || s->protocol >= 31;

    if (o->verbose)
        printf("receiving file list ... ");
    err[0] = '\0';
    rc = flist_recv(&p.fl, &s->in, &fmt, err, sizeof err);
    if (rc != WIRE_OK) {
        if (o->verbose)
            printf("\n");
        if (s->in.err)
            rc = session_stream_failure(s, "receiving file list");
        else if (rc == WIRE_ENOMEM) {
            fprintf(stderr, "arsync: out of memory receiving file list\n");
            rc = RERR_MALLOC;
        } else {
            fprintf(stderr, "arsync: ABORTING: %s\n", err);
            rc = RERR_UNSUPPORTED;
        }
        goto out;
    }
    if (o->verbose)
        printf("done\n");

    if (!dest)
        rc = list_files(&p);
    else {
        fprintf(stderr, "arsync: file transfer is not implemented yet\n");
        rc = RERR_UNSUPPORTED;
        goto out;
    }

    /* With an empty list the server stops straight away (rsync's client
     * doesn't run the transfer phases either), so there's nothing more to
     * exchange and no statistics to show. */
    if (p.fl.count > 0) {
        if ((rc = finish_phases(&p)) != RERR_OK)
            goto out;
        if (o->verbose)
            print_summary(&p);
    }

    /* As in rsync, any file-related error the server reported counts as a
     * partial transfer. */
    io_error = p.fl.io_error | s->remote_io_error | (s->saw_xfer_error ? IOERR_GENERAL : 0);
    if (io_error) {
        rc = io_error & IOERR_VANISHED && !(io_error & IOERR_GENERAL) ? RERR_VANISHED : RERR_PARTIAL;
        fprintf(stderr, "arsync error: %s (code %d)\n",
                rc == RERR_VANISHED ? "some files vanished before they could be transferred"
                                    : "some files/attrs were not transferred (see previous errors)",
                rc);
    }

out:
    flist_free(&p.fl);
    return rc;
}
