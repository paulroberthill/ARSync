/* client.c - the receiving side of a pull: file list, then listing or
 * transfer, then the end-of-run exchange
 *
 * rsync runs the receiving side as two processes, a generator (decides
 * what to ask for, writes requests) and a receiver (reads what arrives).
 * Here both run in one loop. Every request gets exactly one reply (or a
 * MSG_NO_SEND in its place), so the loop lets the generator run up to
 * PULL_WINDOW requests ahead and otherwise reads replies; a read always
 * flushes our pending output first, so neither side can stall.
 *
 * The exchange after the file list (G = we write, R = we read):
 *
 *   G: requests ... NDX_DONE         phase 1
 *   R: replies  ... NDX_DONE
 *   G: redo requests ...             phase 2: files that failed verification
 *   G: NDX_DONE                      end of phase 2
 *   G: NDX_DONE                      no delay-updates phase
 *   G: NDX_DONE                      (protocol >= 31) no late-delete phase
 *   R: redo replies ... NDX_DONE     server finished phase 2
 *   R: NDX_DONE                      server finished sending
 *   R: 5 x varlong                   server's statistics
 *   R: NDX_DONE                      (protocol >= 31) goodbye echo
 *   G: NDX_DONE                      our final goodbye
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "client.h"
#include "output.h"
#include "plat.h"
#include "pull.h"

/* The server's byte counts, already turned round to our point of view: it
 * sends what it read (= what we sent) first, then what it wrote. */
typedef struct server_stats {
    int64_t sent;
    int64_t received;
    int64_t total_size;
    int64_t flist_buildtime;
    int64_t flist_xfertime;
} server_stats;

/* --- shared helpers (pull.h) --- */

/* Joins an AmigaDOS or POSIX directory and a relative name. */
static void path_join(char *buf, size_t bufsize, const char *dir, const char *name)
{
    size_t len = strlen(dir);

    if (len == 0)
        snprintf(buf, bufsize, "%s", name);
    else if (dir[len - 1] == '/' || dir[len - 1] == ':')
        snprintf(buf, bufsize, "%s%s", dir, name);
    else
        snprintf(buf, bufsize, "%s/%s", dir, name);
}

void pull_local_path(const pull_state *p, size_t ndx, char *buf, size_t bufsize)
{
    const char *name = flist_name(&p->fl, &p->fl.files[ndx]);

    if (p->dest_is_file || strcmp(name, ".") == 0)
        snprintf(buf, bufsize, "%s", p->dest);
    else
        path_join(buf, bufsize, p->dest, name);
}

void pull_file_error(pull_state *p, const char *fmt, ...)
{
    va_list ap;

    fflush(stdout);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    p->xfer_error = 1;
}

/* --- flushing and the end-of-run exchange --- */

static int flush(pull_state *p)
{
    if (mux_flush(&p->s->mux) != WIRE_OK || conn_flush(&p->s->conn) != WIRE_OK) {
        if (!p->s->out.err)
            p->s->out.err = p->s->conn.w.err ? p->s->conn.w.err : WIRE_EIO;
        return -1;
    }
    return 0;
}

/* Reads replies until the server's NDX_DONE. */
static int replies_until_done(pull_state *p)
{
    int done = 0, rc;

    while (!done) {
        if ((rc = recv_reply(p, 1, &done)) != RERR_OK)
            return rc;
    }
    return RERR_OK;
}

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

static int run_phases(pull_state *p, server_stats *stats)
{
    ars_reader *in = &p->s->in;
    ars_writer *out = &p->s->out;
    size_t i;
    int rc, done;

    /* Phase 1: everything in the list (nothing when only listing). */
    if (p->listing)
        p->gen_next = p->fl.count;
    while (p->gen_next < p->fl.count || p->outstanding > 0) {
        /* The generator waits while our output is backed up: the server
         * is busy sending, and its data must be read first. */
        while (p->gen_next < p->fl.count && p->outstanding < PULL_WINDOW
               && p->s->conn.out_len <= CONN_OUT_HIGH)
            if ((rc = gen_entry(p, p->gen_next++)) != RERR_OK)
                return rc;
        if (p->outstanding > 0) {
            if ((rc = recv_reply(p, 0, &done)) != RERR_OK)
                return rc;
            if (done) {
                fprintf(stderr, "arsync: protocol error: server ended phase 1 early\n");
                return RERR_PROTOCOL;
            }
        }
    }
    w_ndx(out, &p->gen_ndx, NDX_DONE);
    if (flush(p) < 0)
        return session_stream_failure(p->s, "ending phase 1");
    if ((rc = replies_until_done(p)) != RERR_OK)
        return rc;

    /* Phase 2: ask again for whatever failed verification. */
    p->redoing = 1;
    for (i = 0; i < p->redo_count; i++)
        gen_redo(p, p->redo[i]);
    w_ndx(out, &p->gen_ndx, NDX_DONE);
    w_ndx(out, &p->gen_ndx, NDX_DONE);
    if (p->s->protocol >= 31)
        w_ndx(out, &p->gen_ndx, NDX_DONE);
    if (flush(p) < 0)
        return session_stream_failure(p->s, "ending phase 2");
    if ((rc = replies_until_done(p)) != RERR_OK
        || (rc = expect_done(p, "at end of transfer")) != RERR_OK)
        return rc;

    stats->sent = r_varlong(in, 3);
    stats->received = r_varlong(in, 3);
    stats->total_size = r_varlong(in, 3);
    stats->flist_buildtime = r_varlong(in, 3);
    stats->flist_xfertime = r_varlong(in, 3);
    if (in->err)
        return session_stream_failure(p->s, "reading statistics");

    if (p->s->protocol >= 31 && (rc = expect_done(p, "in goodbye")) != RERR_OK)
        return rc;

    w_ndx(out, &p->gen_ndx, NDX_DONE);
    if (flush(p) < 0)
        return session_stream_failure(p->s, "sending goodbye");
    return RERR_OK;
}

/* --- destination --- */

/* Works out where files go, as rsync does: into an existing directory;
 * into a new directory if there's more than one file or the destination
 * ends in '/'; otherwise the single file is written to that name. */
static int resolve_dest(pull_state *p, const char *dest)
{
    plat_fileinfo st;
    size_t len;
    int rc, trailing;

    if (!*dest)
        dest = ".";
    snprintf(p->dest, sizeof p->dest, "%s", dest);
    len = strlen(p->dest);
    trailing = len > 1 && p->dest[len - 1] == '/';

    rc = plat_stat(p->dest, &st);
    if (rc == PLAT_OK && st.type == PLAT_T_DIR)
        return RERR_OK;
    if (rc == PLAT_OK) {
        if (p->fl.count > 1) {
            fprintf(stderr, "arsync: ERROR: destination must be a directory when copying more than 1 file\n");
            return RERR_FILESELECT;
        }
        if (WIRE_ISDIR(p->fl.files[0].mode)) {
            fprintf(stderr, "arsync: ERROR: cannot overwrite non-directory with a directory\n");
            return RERR_FILESELECT;
        }
        p->dest_is_file = 1;
        return RERR_OK;
    }
    if (rc != PLAT_ENOENT) {
        fprintf(stderr, "arsync: ERROR: cannot stat destination %s: %s\n", p->dest, plat_strerror(rc));
        return RERR_FILESELECT;
    }

    if (p->fl.count > 1 || trailing) {
        if (trailing)
            p->dest[len - 1] = '\0';
        if (!p->o->dry_run && (rc = plat_mkdir(p->dest)) != PLAT_OK) {
            fprintf(stderr, "arsync: mkdir %s failed: %s\n", p->dest, plat_strerror(rc));
            return RERR_FILEIO;
        }
        p->dest_created = 1;
        if (p->o->verbose)
            printf("created directory %s\n", p->dest);
        return RERR_OK;
    }
    p->dest_is_file = 1;
    return RERR_OK;
}

/* With -t, directories get their times last: writing files into them
 * changed their modification times along the way. */
static void set_dir_times(pull_state *p)
{
    char local[FLIST_MAXPATH];
    size_t i;
    int rc;

    if (!p->o->times || p->o->dry_run)
        return;
    for (i = 0; i < p->fl.count; i++) {
        const ars_file *f = &p->fl.files[i];

        if (!f->mode || !WIRE_ISDIR(f->mode) || (p->fstate[i] & FS_SKIPPED))
            continue;
        pull_local_path(p, i, local, sizeof local);
        if ((rc = plat_set_mtime(local, f->mtime, (long)f->mtime_nsec)) != PLAT_OK
            && rc != PLAT_ENOENT)
            pull_file_error(p, "arsync: failed to set times on %s: %s", local, plat_strerror(rc));
    }
}

/* --- reporting --- */

static void print_summary(pull_state *p, const server_stats *stats)
{
    char sent[32], recvd[32], rate[40], size[32];
    int64_t total = stats->sent + stats->received;
    long elapsed = (long)(time(NULL) - p->s->started);
    int64_t speedup;

    /* rsync's rate is bytes / (0.5 + whole seconds), shown with 2 decimals:
     * in hundredths that's bytes * 200 / (1 + 2 * seconds), rounded. */
    comma_num_frac2((total * 200 + elapsed) / (1 + 2 * (int64_t)elapsed), rate);
    printf("\nsent %s bytes  received %s bytes  %s bytes/sec\n",
           comma_num(stats->sent, sent), comma_num(stats->received, recvd), rate);
    speedup = total ? (stats->total_size * 100 + total / 2) / total : 0;
    printf("total size is %s  speedup is %s%s\n", comma_num(stats->total_size, size),
           comma_num_frac2(speedup, rate), p->o->dry_run ? " (DRY RUN)" : "");
}

static void list_files(pull_state *p)
{
    char line[FLIST_MAXPATH + 64];
    size_t i;

    for (i = 0; i < p->fl.count; i++) {
        if (!p->fl.files[i].mode)
            continue;
        format_list_entry(&p->fl, &p->fl.files[i], line, sizeof line);
        printf("%s\n", line);
    }
}

int client_pull(ars_session *s, const ars_options *o, const char *dest)
{
    ars_flist_format fmt;
    server_stats stats;
    pull_state p;
    char err[FLIST_MAXPATH + 64];
    int rc, io_error, show_flist;

    memset(&p, 0, sizeof p);
    p.s = s;
    p.o = o;
    ndx_state_init(&p.gen_ndx);
    ndx_state_init(&p.recv_ndx);
    flist_init(&p.fl);

    fmt.varint_flags = (s->compat_flags & CF_VARINT_FLIST_FLAGS) != 0;
    fmt.safe_flist = (s->compat_flags & CF_SAFE_FLIST) || s->protocol >= 31;

    /* rsync shows this only when directories are being transferred. */
    show_flist = o->verbose && (o->recursive || o->list_only);
    if (show_flist)
        printf("receiving file list ... ");
    err[0] = '\0';
    rc = flist_recv(&p.fl, &s->in, &fmt, err, sizeof err);
    if (rc != WIRE_OK) {
        if (show_flist)
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
    if (show_flist)
        printf("done\n");

    /* With an empty list the server stops straight away (rsync's client
     * doesn't run the transfer phases either). */
    if (p.fl.count == 0)
        goto report;

    if (dest) {
        p.fstate = calloc(p.fl.count, 1);
        p.redo = malloc(p.fl.count * sizeof *p.redo);
        p.buf = malloc(CHUNK_SIZE);
        p.block = malloc(MAX_BLOCK_SIZE);
        p.basis_buf = malloc(BASIS_CACHE);
        if (!p.fstate || !p.redo || !p.buf || !p.block || !p.basis_buf) {
            fprintf(stderr, "arsync: out of memory\n");
            rc = RERR_MALLOC;
            goto out;
        }
        if ((rc = resolve_dest(&p, dest)) != RERR_OK)
            goto out;
    } else {
        p.listing = 1;
        list_files(&p);
    }

    memset(&stats, 0, sizeof stats);
    if ((rc = run_phases(&p, &stats)) != RERR_OK)
        goto out;
    if (dest)
        set_dir_times(&p);
    if (o->verbose)
        print_summary(&p, &stats);

report:
    /* As in rsync, any file-related error counts as a partial transfer. */
    io_error = p.fl.io_error | s->remote_io_error
               | (s->saw_xfer_error || p.xfer_error ? IOERR_GENERAL : 0);
    if (io_error) {
        rc = io_error & IOERR_VANISHED && !(io_error & IOERR_GENERAL) ? RERR_VANISHED : RERR_PARTIAL;
        fflush(stdout);
        fprintf(stderr, "arsync error: %s (code %d)\n",
                rc == RERR_VANISHED ? "some files vanished before they could be transferred"
                                    : "some files/attrs were not transferred (see previous errors)",
                rc);
    }

out:
    recv_cleanup(&p);
    free(p.fstate);
    free(p.redo);
    free(p.buf);
    free(p.block);
    free(p.basis_buf);
    flist_free(&p.fl);
    return rc;
}
