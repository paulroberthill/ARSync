/* wire.h - rsync wire encodings and multiplexed framing
 *
 * All multi-byte values are little-endian on the wire. The codec only ever
 * assembles values byte by byte, so it is endian-neutral and runs unchanged
 * on the big-endian 68k.
 *
 * Readers and writers carry a sticky error: once err is non-zero every
 * further read returns 0 and every further write is dropped, so a caller can
 * decode a whole record and check err once at the end.
 */
#ifndef WIRE_H
#define WIRE_H

#include "arsync.h"

#define WIRE_OK 0
#define WIRE_EOF (-1)      /* source ended before the value was complete */
#define WIRE_EIO (-2)      /* underlying transport failed */
#define WIRE_EPROTO (-3)   /* malformed or out-of-range data */
#define WIRE_ENOMEM (-4)
#define WIRE_EABORT (-5)   /* a message handler asked to stop */

typedef struct ars_reader {
    /* Read exactly n bytes into dst; return WIRE_OK or a WIRE_E* code. */
    int (*read)(void *ctx, uint8_t *dst, size_t n);
    void *ctx;
    int err;
} ars_reader;

typedef struct ars_writer {
    /* Accept all n bytes; return WIRE_OK or a WIRE_E* code. */
    int (*write)(void *ctx, const uint8_t *src, size_t n);
    void *ctx;
    int err;
} ars_writer;

/* --- primitive values --- */

void r_bytes(ars_reader *r, void *dst, size_t n);
uint8_t r_byte(ars_reader *r);
int32_t r_int32(ars_reader *r);
int64_t r_longint(ars_reader *r);
int32_t r_varint(ars_reader *r);
int64_t r_varlong(ars_reader *r, int min_bytes);
/* Reads a length-prefixed string, NUL-terminates it in buf.
 * Returns its length, or -1 (and sets r->err) if it doesn't fit. */
int r_vstring(ars_reader *r, char *buf, size_t bufsize);

void w_bytes(ars_writer *w, const void *src, size_t n);
void w_byte(ars_writer *w, uint8_t v);
void w_int32(ars_writer *w, int32_t v);
void w_longint(ars_writer *w, int64_t v);
void w_varint(ars_writer *w, int32_t v);
void w_varlong(ars_writer *w, int64_t v, int min_bytes);
void w_vstring(ars_writer *w, const char *s, size_t len);

/* --- file-list indexes (protocol >= 30 delta encoding) ---
 * Each direction of a connection keeps its own state. */

#define NDX_DONE (-1)

typedef struct ars_ndx_state {
    int32_t prev_positive;
    int32_t prev_negative;
} ars_ndx_state;

void ndx_state_init(ars_ndx_state *st);
void w_ndx(ars_writer *w, ars_ndx_state *st, int32_t ndx);
int32_t r_ndx(ars_reader *r, ars_ndx_state *st);

/* --- growable memory buffer usable as both source and sink --- */

typedef struct ars_membuf {
    uint8_t *data;
    size_t len;   /* bytes stored */
    size_t cap;
    size_t pos;   /* read position */
} ars_membuf;

void membuf_init(ars_membuf *mb);
void membuf_free(ars_membuf *mb);
void membuf_reader(ars_reader *r, ars_membuf *mb);
void membuf_writer(ars_writer *w, ars_membuf *mb);

/* --- multiplexing --- */

#define MPLEX_BASE 7
#define MPLEX_MAX_LEN 0xFFFFFF
#define MPLEX_MAX_MSG 32768  /* largest non-data message we accept */

enum {
    MSG_DATA = 0,
    MSG_ERROR_XFER = 1,
    MSG_INFO = 2,
    MSG_ERROR = 3,
    MSG_WARNING = 4,
    MSG_ERROR_SOCKET = 5,
    MSG_LOG = 6,
    MSG_CLIENT = 7,
    MSG_ERROR_UTF8 = 8,
    MSG_REDO = 9,
    MSG_STATS = 10,
    MSG_IO_ERROR = 22,
    MSG_IO_TIMEOUT = 33,
    MSG_NOOP = 42,
    MSG_ERROR_EXIT = 86,
    MSG_SUCCESS = 100,
    MSG_DELETED = 101,
    MSG_NO_SEND = 102
};

void mplex_encode_header(uint8_t out[4], int tag, size_t len);
/* Returns WIRE_OK or WIRE_EPROTO if the header isn't a multiplex header. */
int mplex_decode_header(const uint8_t in[4], int *tag, size_t *len);

/* Called for every non-data message; return WIRE_OK to keep reading or a
 * WIRE_E* code to abort the read in progress. */
typedef int (*ars_msg_fn)(void *ctx, int tag, const uint8_t *payload, size_t len);

/* Demultiplexer: presents the MSG_DATA payloads of a framed stream as a
 * plain byte stream, dispatching other messages to on_msg as it goes. */
typedef struct ars_demux {
    ars_reader *raw;
    size_t data_left;     /* unread bytes of the current MSG_DATA frame */
    ars_msg_fn on_msg;
    void *msg_ctx;
    uint8_t *msgbuf;      /* MPLEX_MAX_MSG bytes, allocated on first use */
} ars_demux;

void demux_init(ars_demux *d, ars_reader *raw, ars_msg_fn on_msg, void *msg_ctx);
void demux_free(ars_demux *d);
void demux_reader(ars_reader *r, ars_demux *d);

/* Multiplexer: wraps plain writes into MSG_DATA frames. Data is buffered
 * and only framed on a full buffer, mux_flush() or mux_send_msg(). */
#define MUX_BUFSIZE 32768

typedef struct ars_mux {
    ars_writer *raw;
    uint8_t *buf;         /* MUX_BUFSIZE bytes */
    size_t len;
} ars_mux;

int mux_init(ars_mux *m, ars_writer *raw);   /* WIRE_OK or WIRE_ENOMEM */
void mux_free(ars_mux *m);
void mux_writer(ars_writer *w, ars_mux *m);
int mux_flush(ars_mux *m);
/* Flushes pending data first so ordering is preserved. */
int mux_send_msg(ars_mux *m, int tag, const void *payload, size_t len);

#endif
