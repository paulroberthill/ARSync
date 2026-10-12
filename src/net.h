/* net.h - buffered, deadlock-free connection over a non-blocking socket
 *
 * Presents the socket as an ars_reader/ars_writer pair with blocking
 * semantics, but every wait services both directions: while waiting for
 * input, pending output keeps draining, and while waiting to send, incoming
 * data is buffered. Neither side can then stall the connection by filling
 * its socket buffers while the other does the same. This is the
 * single-process replacement for rsync's forked generator/receiver.
 */
#ifndef NET_H
#define NET_H

#include "wire.h"

#define CONN_IN_MAX (1024L * 1024)   /* input buffered while waiting to send */
#define CONN_OUT_HIGH 32768          /* output held before forcing a send */

typedef struct ars_conn {
    int sock;
    long timeout_ms;    /* per wait; < 0 = none */
    uint8_t *in;
    size_t in_pos, in_len, in_cap;
    uint8_t *out;
    size_t out_len, out_cap;
    int eof;
    int plat_err;       /* PLAT_E* behind the last failure, for reporting */
    /* Called before waiting for input, so that output buffered in layers
     * above (the multiplexer) is sent first. Optional. */
    int (*before_wait)(void *ctx);
    void *before_wait_ctx;
    ars_reader r;       /* raw byte stream from the peer */
    ars_writer w;       /* raw byte stream to the peer */
} ars_conn;

/* Connects; returns PLAT_OK or a PLAT_E* code. timeout is in seconds,
 * 0 = none. */
int conn_open(ars_conn *c, const char *host, int port, int timeout);
void conn_close(ars_conn *c);

/* Sends everything buffered. WIRE_OK or a WIRE_E* code. */
int conn_flush(ars_conn *c);

/* Reads one '\n'-terminated line, dropping '\r' and the newline, into buf.
 * Returns WIRE_OK, WIRE_EOF, or WIRE_EPROTO (line too long or NUL byte). */
int conn_read_line(ars_conn *c, char *buf, size_t bufsize);

/* Describes the last failure on c, e.g. "timed out". */
const char *conn_strerror(const ars_conn *c);

#endif
