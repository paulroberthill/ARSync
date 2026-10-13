/* session.c - rsync daemon session: connect, greeting, module requests,
 * authentication and the protocol setup that precedes a transfer */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "session.h"
#include "auth.h"
#include "digest.h"
#include "plat.h"

#define LINE_MAX 4096

int negotiate_protocol(int ours, int our_sub, int remote, int remote_sub)
{
    if (ours > remote)
        return remote_sub ? remote - 1 : remote;
    if (ours == remote)
        return remote_sub != our_sub ? ours - 1 : ours;
    return our_sub ? ours - 1 : ours;
}

int parse_greeting(const char *line, int *protocol, int *sub, char *digests, size_t digestsize)
{
    const char *p;
    long v;

    if (strncmp(line, "@RSYNCD: ", 9) != 0)
        return -1;
    p = line + 9;
    if (*p < '0' || *p > '9')
        return -1;
    for (v = 0; *p >= '0' && *p <= '9' && v < 100000; p++)
        v = v * 10 + (*p - '0');
    *protocol = (int)v;

    *sub = -1;   /* absent */
    if (*p == '.') {
        p++;
        if (*p < '0' || *p > '9')
            return -1;
        for (v = 0; *p >= '0' && *p <= '9' && v < 100000; p++)
            v = v * 10 + (*p - '0');
        *sub = (int)v;
    }

    digests[0] = '\0';
    if (*p == ' ') {
        p++;
        if (strlen(p) >= digestsize)
            return -1;
        strcpy(digests, p);
    } else if (*p) {
        return -1;
    }
    return 0;
}

/* Maps a connection failure onto an exit code, after reporting it. */
static int conn_failure(ars_session *s, const char *what)
{
    fprintf(stderr, "arsync: %s: %s\n", what, conn_strerror(&s->conn));
    switch (s->conn.plat_err) {
    case PLAT_EBREAK:
        return RERR_SIGNAL;
    case PLAT_ETIMEOUT:
        return RERR_TIMEOUT;
    case PLAT_ENOMEM:
        return RERR_MALLOC;
    default:
        return s->conn.r.err == WIRE_EPROTO ? RERR_PROTOCOL : RERR_SOCKETIO;
    }
}

static int send_line(ars_session *s, const char *line)
{
    w_bytes(&s->conn.w, line, strlen(line));
    w_byte(&s->conn.w, '\n');
    return conn_flush(&s->conn);
}

int session_open(ars_session *s, const ars_remote *rem, const ars_options *o)
{
    char line[LINE_MAX];
    int port = rem->port ? rem->port : o->port ? o->port : RSYNC_PORT;
    int rc;

    memset(s, 0, sizeof *s);
    s->started = time(NULL);
    s->remote_exit = -1;
    if (o->verbose > 1)
        printf("opening tcp connection to %s port %d\n", rem->host, port);
    rc = conn_open(&s->conn, rem->host, port, o->timeout);
    if (rc != PLAT_OK) {
        fprintf(stderr, "arsync: failed to connect to %s: %s\n", rem->host, plat_strerror(rc));
        return rc == PLAT_EBREAK ? RERR_SIGNAL : rc == PLAT_ENOMEM ? RERR_MALLOC : RERR_SOCKETIO;
    }
    s->connected = 1;

    /* Both sides send their greeting without waiting for the other's. */
    snprintf(line, sizeof line, "@RSYNCD: %d.0 %s", ARS_PROTOCOL_VERSION, ARS_AUTH_DIGESTS);
    if (send_line(s, line) != WIRE_OK)
        return conn_failure(s, "failed to send greeting");

    if ((rc = conn_read_line(&s->conn, line, sizeof line)) != WIRE_OK)
        return conn_failure(s, "did not see server greeting");
    if (parse_greeting(line, &s->remote_protocol, &s->remote_sub,
                       s->remote_digests, sizeof s->remote_digests) < 0) {
        fprintf(stderr, "arsync: server sent \"%s\" rather than greeting\n", line);
        return RERR_STARTCLIENT;
    }
    if (s->remote_sub < 0) {
        if (s->remote_protocol >= 30) {
            fprintf(stderr, "arsync: the server omitted the subprotocol value: %s\n", line);
            return RERR_PROTOCOL;
        }
        s->remote_sub = 0;
    }
    if (s->remote_protocol > 31 && !s->remote_digests[0]) {
        fprintf(stderr, "arsync: the server omitted the digest name list: %s\n", line);
        return RERR_PROTOCOL;
    }

    s->protocol = negotiate_protocol(ARS_PROTOCOL_VERSION, 0, s->remote_protocol, s->remote_sub);
    if (o->verbose > 1)
        printf("server protocol %d.%d, using protocol %d\n",
               s->remote_protocol, s->remote_sub, s->protocol);
    if (s->protocol < ARS_MIN_PROTOCOL) {
        fprintf(stderr, "arsync: server speaks protocol %d; at least %d (rsync 3.0) is needed\n",
                s->remote_protocol, ARS_MIN_PROTOCOL);
        return RERR_PROTOCOL;
    }
    return RERR_OK;
}

int session_list_modules(ars_session *s, const ars_options *o)
{
    char line[LINE_MAX];

    /* An empty module name asks for the module list. */
    if (send_line(s, "") != WIRE_OK)
        return conn_failure(s, "failed to request module list");

    for (;;) {
        if (conn_read_line(&s->conn, line, sizeof line) != WIRE_OK)
            return conn_failure(s, "didn't get server startup line");
        if (strcmp(line, "@RSYNCD: EXIT") == 0)
            return RERR_OK;
        if (strncmp(line, "@ERROR", 6) == 0) {
            fprintf(stderr, "%s\n", line);
            return RERR_STARTCLIENT;
        }
        /* MOTD lines and module lines can't be told apart; like rsync,
         * --no-motd hides both. */
        if (!o->no_motd)
            printf("%s\n", line);
    }
}

/* --- module request and authentication --- */

int build_server_args(const ars_options *o, const ars_remote *rem, int push,
                      const char **args, char *buf, size_t bufsize)
{
    char module[256];
    size_t pos = 0;
    int n = 0, i;

    args[n++] = "--server";
    if (!push)
        args[n++] = "--sender";

    /* One bundle of single-letter options, ending in our capabilities. */
    args[n++] = buf + pos;
    buf[pos++] = '-';
    for (i = 0; i < o->verbose && i < 4; i++)
        buf[pos++] = 'v';
    if (o->dry_run)
        buf[pos++] = 'n';
    if (o->list_only && !o->recursive)
        buf[pos++] = 'd';        /* list a directory's contents, not just itself */
    if (o->whole_file)
        buf[pos++] = 'W';
    if (o->times)
        buf[pos++] = 't';
    if (o->recursive)
        buf[pos++] = 'r';
    /* "zlibx" by name has no letter: rsync sends --new-compress alone. */
    if (o->compress && o->compress_choice != CPRES_ZLIBX)
        buf[pos++] = 'z';
    pos += (size_t)snprintf(buf + pos, bufsize - pos, "e.%s", ARS_CLIENT_INFO) + 1;

    if (o->list_only > 1)
        args[n++] = "--list-only";
    /* When pulling we do the deleting; a receiving server needs telling. */
    if (push && o->delete_excluded)
        args[n++] = "--delete-excluded";
    else if (push && o->delete_mode)
        args[n++] = "--delete";
    if (o->compress && o->compress_choice == CPRES_ZLIB)
        args[n++] = "--old-compress";
    else if (o->compress && o->compress_choice == CPRES_ZLIBX)
        args[n++] = "--new-compress";
    if (o->compress && o->compress_level >= 0) {
        args[n++] = buf + pos;
        pos += (size_t)snprintf(buf + pos, bufsize - pos, "--compress-level=%d", o->compress_level) + 1;
    }
    args[n++] = ".";

    /* A bare module name is sent as "module/", as rsync does. */
    remote_module(rem, module, sizeof module);
    if (strcmp(rem->path, module) == 0) {
        args[n++] = buf + pos;
        pos += (size_t)snprintf(buf + pos, bufsize - pos, "%s/", module) + 1;
    } else {
        args[n++] = rem->path;
    }
    return n;
}

static int authenticate(ars_session *s, const ars_remote *rem, const ars_options *o,
                        const char *challenge)
{
    char password[256], response[64], line[512];
    const char *user = rem->user, *env;
    int digest, rc;

    if (!*user && !(user = getenv("USER")) && !(user = getenv("LOGNAME")))
        user = "";
    if (!*user)
        user = "nobody";

    /* Protocol 32 servers list their digests; older ones always use md5. */
    digest = s->remote_digests[0] ? choose_digest(ARS_AUTH_DIGESTS, s->remote_digests)
                                  : DIGEST_MD5;
    if (!digest) {
        fprintf(stderr, "arsync: failed to negotiate a daemon auth checksum"
                " (server offers: %s, we support: %s)\n", s->remote_digests, ARS_AUTH_DIGESTS);
        return RERR_UNSUPPORTED;
    }

    if (o->password_file) {
        if (read_password_file(o->password_file, password, sizeof password) < 0) {
            fprintf(stderr, "arsync: failed to read a password from %s\n", o->password_file);
            return RERR_SYNTAX;
        }
    } else if ((env = getenv("RSYNC_PASSWORD")) != NULL) {
        snprintf(password, sizeof password, "%s", env);
    } else if ((rc = plat_read_password("Password: ", password, sizeof password)) != PLAT_OK) {
        return rc == PLAT_EBREAK ? RERR_SIGNAL : RERR_STARTCLIENT;
    }

    auth_response(digest, password, challenge, response);
    memset(password, 0, sizeof password);
    if (o->verbose > 1)
        printf("authenticating as %s using %s\n", user, digest == DIGEST_MD5 ? "md5" : "md4");
    snprintf(line, sizeof line, "%s %s", user, response);
    if (send_line(s, line) != WIRE_OK)
        return conn_failure(s, "failed to send authentication");
    return RERR_OK;
}

int session_request(ars_session *s, const ars_remote *rem, const ars_options *o,
                    const char **args, int nargs)
{
    char module[256], line[LINE_MAX];
    int i, rc;

    remote_module(rem, module, sizeof module);
    if (send_line(s, module) != WIRE_OK)
        return conn_failure(s, "failed to request module");

    for (;;) {
        if (conn_read_line(&s->conn, line, sizeof line) != WIRE_OK)
            return conn_failure(s, "didn't get server startup line");
        if (strncmp(line, "@RSYNCD: AUTHREQD ", 18) == 0) {
            if ((rc = authenticate(s, rem, o, line + 18)) != RERR_OK)
                return rc;
            continue;
        }
        if (strcmp(line, "@RSYNCD: OK") == 0)
            break;
        if (strncmp(line, "@ERROR", 6) == 0) {
            fprintf(stderr, "%s\n", line);
            return RERR_STARTCLIENT;
        }
        if (strcmp(line, "@RSYNCD: EXIT") == 0) {
            fprintf(stderr, "arsync: server ended the session\n");
            return RERR_STARTCLIENT;
        }
        if (!o->no_motd)
            printf("%s\n", line);
    }

    if (o->verbose > 2) {
        printf("sending daemon args:");
        for (i = 0; i < nargs; i++)
            printf(" %s", args[i]);
        printf("\n");
    }
    /* From protocol 30 the arguments are NUL-terminated, ending with an
     * empty one. */
    for (i = 0; i < nargs; i++)
        w_bytes(&s->conn.w, args[i], strlen(args[i]) + 1);
    w_byte(&s->conn.w, 0);
    if (conn_flush(&s->conn) != WIRE_OK)
        return conn_failure(s, "failed to send arguments");
    return RERR_OK;
}

/* --- protocol setup and multiplexing --- */

static int32_t le32(const uint8_t *p)
{
    return (int32_t)((uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24);
}

static int on_message(void *ctx, int tag, const uint8_t *payload, size_t len)
{
    ars_session *s = ctx;

    switch (tag) {
    case MSG_INFO:
    case MSG_CLIENT:
        fwrite(payload, 1, len, stdout);
        fflush(stdout);
        return WIRE_OK;
    case MSG_ERROR_XFER:
        s->saw_xfer_error = 1;
        /* fall through */
    case MSG_ERROR:
        s->saw_error = 1;
        /* fall through */
    case MSG_WARNING:
        fflush(stdout);
        fwrite(payload, 1, len, stderr);
        return WIRE_OK;
    case MSG_IO_ERROR:
        if (len != 4)
            return WIRE_EPROTO;
        s->remote_io_error |= le32(payload);
        return WIRE_OK;
    case MSG_ERROR_EXIT:
        s->remote_exit = len == 4 ? le32(payload) : RERR_PARTIAL;
        return WIRE_EABORT;
    case MSG_NO_SEND:
        /* Sent instead of the reply to a request: the file couldn't be
         * opened. It can arrive in the middle of another file's data, so
         * it's only counted here; a wait for the next reply is cut short,
         * since that reply may never come. */
        if (len != 4)
            return WIRE_EPROTO;
        s->no_send_pending++;
        return s->awaiting_reply ? WIRE_EINTR : WIRE_OK;
    case MSG_DELETED:
        /* A receiving server deleted something (--delete); a trailing NUL
         * marks a directory. */
        if (s->verbose && len > 0) {
            int dir = len > 1 && payload[len - 1] == '\0';

            printf("deleting %.*s%s\n", (int)(dir ? len - 1 : len), (const char *)payload,
                   dir ? "/" : "");
        }
        return WIRE_OK;
    case MSG_LOG:
    case MSG_NOOP:
    case MSG_IO_TIMEOUT:
    case MSG_SUCCESS:       /* only meaningful with options we don't use */
        return WIRE_OK;
    default:
        return WIRE_EPROTO;
    }
}

static int flush_mux_hook(void *ctx)
{
    return mux_flush(ctx);
}

static int session_flush(ars_session *s)
{
    int rc;

    if (s->multiplexing && (rc = mux_flush(&s->mux)) != WIRE_OK)
        return s->out.err = rc;
    return conn_flush(&s->conn);
}

/* The first method in our list that the server also offers. */
static int choose_compression(const char *theirs)
{
    const char *p = theirs, *end;
    size_t len;
    int best = CPRES_NONE;

    while (*p) {
        while (*p == ' ' || *p == '\t')
            p++;
        for (end = p; *end && *end != ' ' && *end != '\t'; end++)
            ;
        len = (size_t)(end - p);
        if (len == 5 && memcmp(p, "zlibx", 5) == 0)
            best = CPRES_ZLIBX;
        else if (len == 4 && memcmp(p, "zlib", 4) == 0 && best == CPRES_NONE)
            best = CPRES_ZLIB;
        p = end;
    }
    return best;
}

int session_setup_protocol(ars_session *s, const ars_options *o, int push)
{
    char theirs[256];
    int negotiate_compression;
    ars_reader *r = &s->conn.r;
    ars_writer *w = &s->conn.w;

    s->compat_flags = r_varint(r);
    if (r->err)
        return conn_failure(s, "failed to read compatibility flags");
    if (s->compat_flags & CF_INC_RECURSE) {
        fprintf(stderr, "arsync: server enabled incremental recursion, which we didn't offer\n");
        return RERR_PROTOCOL;
    }

    /* Servers that understood our 'v' capability negotiate the checksum;
     * older protocol-30/31 ones always use md5. */
    s->checksum_type = DIGEST_MD5;
    s->verbose = o->verbose;
    /* Compression: as chosen with --compress-choice, negotiated, or (from
     * servers too old to negotiate) zlib. */
    s->compression = !o->compress ? CPRES_NONE
                     : o->compress_choice ? o->compress_choice : CPRES_ZLIB;
    negotiate_compression = o->compress && !o->compress_choice
                            && (s->compat_flags & CF_VARINT_FLIST_FLAGS);
    if (s->compat_flags & CF_VARINT_FLIST_FLAGS) {
        /* Both lists go out before either answer is read. */
        w_vstring(w, ARS_CHECKSUMS, strlen(ARS_CHECKSUMS));
        if (negotiate_compression)
            w_vstring(w, ARS_COMPRESSIONS, strlen(ARS_COMPRESSIONS));
        if (conn_flush(&s->conn) != WIRE_OK)
            return conn_failure(s, "failed to send checksum list");
        if (r_vstring(r, theirs, sizeof theirs) < 0)
            return conn_failure(s, "failed to read checksum list");
        if (!(s->checksum_type = choose_digest(ARS_CHECKSUMS, theirs))) {
            fprintf(stderr, "arsync: failed to negotiate a checksum choice"
                    " (server offers: %s, we support: %s)\n", theirs, ARS_CHECKSUMS);
            return RERR_UNSUPPORTED;
        }
        if (negotiate_compression) {
            if (r_vstring(r, theirs, sizeof theirs) < 0)
                return conn_failure(s, "failed to read compression list");
            if (!(s->compression = choose_compression(theirs))) {
                fprintf(stderr, "arsync: failed to negotiate a compression choice"
                        " (server offers: %s, we support: %s)\n", theirs, ARS_COMPRESSIONS);
                return RERR_UNSUPPORTED;
            }
        }
    }

    s->checksum_seed = r_int32(r);
    if (r->err)
        return conn_failure(s, "failed to read checksum seed");

    if (o->verbose > 1)
        printf("compat flags 0x%lx, checksum %s, compression %s, seed %ld\n", (long)s->compat_flags,
               s->checksum_type == DIGEST_MD5 ? "md5" : "md4",
               s->compression == CPRES_ZLIBX ? "zlibx" : s->compression == CPRES_ZLIB ? "zlib" : "none",
               (long)s->checksum_seed);

    /* Everything from here on is multiplexed in both directions. */
    demux_init(&s->demux, &s->conn.r, on_message, s);
    demux_reader(&s->in, &s->demux);
    if (mux_init(&s->mux, &s->conn.w) != WIRE_OK) {
        fprintf(stderr, "arsync: out of memory\n");
        return RERR_MALLOC;
    }
    mux_writer(&s->out, &s->mux);
    s->multiplexing = 1;
    /* Anything we've queued must go out before we wait for an answer. */
    s->conn.before_wait = flush_mux_hook;
    s->conn.before_wait_ctx = &s->mux;

    /* A receiving client sends its filter rules, for the server to apply
     * when building its list. A sending client applies them itself, and
     * sends them only when the server will be deleting, so it knows what
     * to leave alone; with --delete-excluded that's nothing. */
    if (!push || o->delete_mode) {
        if (push && o->delete_excluded)
            w_int32(&s->out, 0);
        else
            filters_send(&o->filters, &s->out);
        if (session_flush(s) != WIRE_OK)
            return session_stream_failure(s, "failed to send filter list");
    }
    return RERR_OK;
}

int session_wait_reply(ars_session *s)
{
    int rc;

    if (s->in.err)
        return s->in.err;
    s->awaiting_reply = 1;
    rc = demux_fill(&s->demux);
    s->awaiting_reply = 0;
    if (rc != WIRE_OK && rc != WIRE_EINTR)
        s->in.err = rc;
    return rc;
}

int session_stream_failure(ars_session *s, const char *what)
{
    int err = s->in.err ? s->in.err : s->out.err;

    if (err == WIRE_EABORT && s->remote_exit >= 0)
        return s->remote_exit ? s->remote_exit : RERR_PARTIAL;
    if (err == WIRE_EOF) {
        fprintf(stderr, "arsync: connection unexpectedly closed (%s)\n", what);
        return s->saw_error ? RERR_PARTIAL : RERR_STREAMIO;
    }
    if (err == WIRE_EPROTO) {
        fprintf(stderr, "arsync: protocol error (%s)\n", what);
        return RERR_PROTOCOL;
    }
    if (err == WIRE_ENOMEM) {
        fprintf(stderr, "arsync: out of memory (%s)\n", what);
        return RERR_MALLOC;
    }
    return conn_failure(s, what);
}

void session_close(ars_session *s)
{
    if (s->multiplexing) {
        demux_free(&s->demux);
        mux_free(&s->mux);
        s->multiplexing = 0;
    }
    if (s->connected)
        conn_close(&s->conn);
    s->connected = 0;
}
