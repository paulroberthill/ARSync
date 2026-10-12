/* net.c - buffered, deadlock-free connection over a non-blocking socket */
#include <stdlib.h>
#include <string.h>

#include "net.h"
#include "plat.h"

#define RECV_CHUNK 32768

static int fail(ars_conn *c, int plat_err)
{
    c->plat_err = plat_err;
    return plat_err == PLAT_ENOMEM ? WIRE_ENOMEM : WIRE_EIO;
}

/* Makes room for at least `want` more input bytes, compacting first and
 * growing up to CONN_IN_MAX. Returns the free space. */
static size_t in_room(ars_conn *c, size_t want)
{
    size_t cap;
    uint8_t *p;

    if (c->in_pos > 0) {
        memmove(c->in, c->in + c->in_pos, c->in_len - c->in_pos);
        c->in_len -= c->in_pos;
        c->in_pos = 0;
    }
    if (c->in_cap - c->in_len < want && c->in_cap < CONN_IN_MAX) {
        cap = c->in_cap * 2;
        while (cap - c->in_len < want && cap < CONN_IN_MAX)
            cap *= 2;
        if (cap > CONN_IN_MAX)
            cap = CONN_IN_MAX;
        if ((p = realloc(c->in, cap)) != NULL) {
            c->in = p;
            c->in_cap = cap;
        }
    }
    return c->in_cap - c->in_len;
}

/* One round of I/O: waits until the socket can make progress in a direction
 * we care about, then moves as much data as it can both ways. Input is
 * always accepted while there's room, so a peer blocked on sending to us
 * never deadlocks against our own pending output. */
static int pump(ars_conn *c, int need_input)
{
    int want_read, want_write, rd, wr, rc;
    size_t room;
    long n;

    room = c->eof ? 0 : in_room(c, need_input ? RECV_CHUNK : 1);
    want_read = room > 0;
    want_write = c->out_len > 0;
    if (!want_read && !want_write)
        return need_input ? fail(c, PLAT_ENOMEM) : WIRE_OK;

    rc = plat_sock_wait(c->sock, want_read, want_write, c->timeout_ms, &rd, &wr);
    if (rc != PLAT_OK)
        return fail(c, rc);

    if (rd && want_read) {
        n = plat_sock_recv(c->sock, c->in + c->in_len, room < RECV_CHUNK ? room : RECV_CHUNK);
        if (n > 0) {
            c->in_len += (size_t)n;
            c->bytes_in += n;
        }
        else if (n == 0)
            c->eof = 1;
        else if (n != PLAT_EAGAIN)
            return fail(c, (int)n);
    }
    if (wr && want_write) {
        n = plat_sock_send(c->sock, c->out, c->out_len);
        if (n > 0) {
            memmove(c->out, c->out + n, c->out_len - (size_t)n);
            c->out_len -= (size_t)n;
            c->bytes_out += n;
        } else if (n != PLAT_EAGAIN) {
            return fail(c, (int)n);
        }
    }
    return WIRE_OK;
}

static int conn_read(void *ctx, uint8_t *dst, size_t n)
{
    ars_conn *c = ctx;
    size_t avail;
    int rc;

    while (n > 0) {
        avail = c->in_len - c->in_pos;
        if (avail > 0) {
            if (avail > n)
                avail = n;
            memcpy(dst, c->in + c->in_pos, avail);
            c->in_pos += avail;
            dst += avail;
            n -= avail;
            continue;
        }
        if (c->eof)
            return WIRE_EOF;
        if (c->before_wait && (rc = c->before_wait(c->before_wait_ctx)) != WIRE_OK)
            return rc;
        if ((rc = pump(c, 1)) != WIRE_OK)
            return rc;
    }
    return WIRE_OK;
}

static int conn_write(void *ctx, const uint8_t *src, size_t n)
{
    ars_conn *c = ctx;
    size_t cap;
    uint8_t *p;
    int rc;

    if (c->out_cap - c->out_len < n) {
        cap = c->out_cap;
        while (cap - c->out_len < n)
            cap *= 2;
        if (!(p = realloc(c->out, cap)))
            return fail(c, PLAT_ENOMEM);
        c->out = p;
        c->out_cap = cap;
    }
    memcpy(c->out + c->out_len, src, n);
    c->out_len += n;

    /* Push data out while there's room to take in what the peer sends
     * meanwhile. With the input buffer full, waiting here could deadlock
     * (the peer may be blocked sending to us), so keep the rest buffered
     * and let the caller read first. */
    while (c->out_len > CONN_OUT_HIGH && (c->block_writes || in_room(c, 1) > 0)) {
        if ((rc = pump(c, 0)) != WIRE_OK)
            return rc;
    }
    return WIRE_OK;
}

int conn_open(ars_conn *c, const char *host, int port, int timeout)
{
    int rc;

    memset(c, 0, sizeof *c);
    c->sock = -1;
    c->timeout_ms = timeout > 0 ? timeout * 1000L : -1;
    c->in_cap = RECV_CHUNK * 2;
    c->out_cap = CONN_OUT_HIGH * 2;
    c->in = malloc(c->in_cap);
    c->out = malloc(c->out_cap);
    if (!c->in || !c->out) {
        conn_close(c);
        return PLAT_ENOMEM;
    }
    if ((rc = plat_tcp_connect(host, port, &c->sock)) != PLAT_OK) {
        conn_close(c);
        return rc;
    }
    c->r.read = conn_read;
    c->r.ctx = c;
    c->w.write = conn_write;
    c->w.ctx = c;
    return PLAT_OK;
}

void conn_close(ars_conn *c)
{
    if (c->sock >= 0)
        plat_sock_close(c->sock);
    c->sock = -1;
    free(c->in);
    free(c->out);
    c->in = c->out = NULL;
}

int conn_flush(ars_conn *c)
{
    int rc;

    if (c->w.err)
        return c->w.err;
    while (c->out_len > 0) {
        if ((rc = pump(c, 0)) != WIRE_OK)
            return c->w.err = rc;
    }
    return WIRE_OK;
}

int conn_read_line(ars_conn *c, char *buf, size_t bufsize)
{
    size_t len = 0;
    uint8_t ch;

    for (;;) {
        r_bytes(&c->r, &ch, 1);
        if (c->r.err)
            return c->r.err;
        if (ch == '\n')
            break;
        if (ch == '\0' || len + 1 >= bufsize)
            return WIRE_EPROTO;
        if (ch != '\r')
            buf[len++] = (char)ch;
    }
    buf[len] = '\0';
    return WIRE_OK;
}

const char *conn_strerror(const ars_conn *c)
{
    if (c->r.err == WIRE_EOF)
        return "connection closed by server";
    if (c->r.err == WIRE_EPROTO || c->w.err == WIRE_EPROTO)
        return "protocol error";
    return plat_strerror(c->plat_err ? c->plat_err : PLAT_EIO);
}
