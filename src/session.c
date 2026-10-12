/* session.c - rsync daemon session: connect, greeting, module requests */
#include <stdio.h>
#include <string.h>

#include "session.h"
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

void session_close(ars_session *s)
{
    if (s->connected)
        conn_close(&s->conn);
    s->connected = 0;
}
