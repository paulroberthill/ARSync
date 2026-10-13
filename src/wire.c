/* wire.c - rsync wire encodings and multiplexed framing */
#include <stdlib.h>
#include <string.h>

#include "wire.h"

/* ------------------------------------------------------------------ */
/* Raw byte access with sticky errors                                  */

void r_bytes(ars_reader *r, void *dst, size_t n)
{
    int rc;

    if (r->err) {
        memset(dst, 0, n);
        return;
    }
    if (n == 0)
        return;
    rc = r->read(r->ctx, dst, n);
    if (rc != WIRE_OK) {
        r->err = rc;
        memset(dst, 0, n);
    }
}

void w_bytes(ars_writer *w, const void *src, size_t n)
{
    int rc;

    if (w->err || n == 0)
        return;
    rc = w->write(w->ctx, src, n);
    if (rc != WIRE_OK)
        w->err = rc;
}

static uint32_t get_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static void put_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

/* ------------------------------------------------------------------ */
/* Fixed-size values                                                   */

uint8_t r_byte(ars_reader *r)
{
    uint8_t b;

    r_bytes(r, &b, 1);
    return b;
}

void w_byte(ars_writer *w, uint8_t v)
{
    w_bytes(w, &v, 1);
}

uint16_t r_uint16(ars_reader *r)
{
    uint8_t b[2];

    r_bytes(r, b, 2);
    return (uint16_t)(b[0] | b[1] << 8);
}

void w_uint16(ars_writer *w, uint16_t v)
{
    uint8_t b[2];

    b[0] = (uint8_t)v;
    b[1] = (uint8_t)(v >> 8);
    w_bytes(w, b, 2);
}

int32_t r_int32(ars_reader *r)
{
    uint8_t b[4];

    r_bytes(r, b, 4);
    return (int32_t)get_le32(b);
}

void w_int32(ars_writer *w, int32_t v)
{
    uint8_t b[4];

    put_le32(b, (uint32_t)v);
    w_bytes(w, b, 4);
}

/* A non-negative value that fits in 31 bits is a plain int32; anything else
 * is the marker 0xFFFFFFFF followed by the full 64-bit value. */
int64_t r_longint(ars_reader *r)
{
    uint8_t b[8];
    int32_t v = r_int32(r);

    if (v != -1)
        return v;
    r_bytes(r, b, 8);
    return (int64_t)((uint64_t)get_le32(b) | (uint64_t)get_le32(b + 4) << 32);
}

void w_longint(ars_writer *w, int64_t v)
{
    uint8_t b[12];

    if (v >= 0 && v <= 0x7FFFFFFF) {
        w_int32(w, (int32_t)v);
        return;
    }
    put_le32(b, 0xFFFFFFFFu);
    put_le32(b + 4, (uint32_t)v);
    put_le32(b + 8, (uint32_t)((uint64_t)v >> 32));
    w_bytes(w, b, 12);
}

/* ------------------------------------------------------------------ */
/* Variable-length integers                                            */
/*
 * A value of `size` bytes (4 for varint, 8 for varlong) is sent as at least
 * `min` bytes: a lead byte followed by the low-order bytes, little-endian.
 * The lead byte starts with one 1-bit per byte beyond `min`, then a 0-bit;
 * its remaining low bits carry the value's most significant byte when that
 * byte is small enough to fit, saving a byte.
 */

static size_t encode_var(uint8_t *out, uint64_t v, int size, int min)
{
    uint8_t le[8];
    int nbytes, i, extra, room;
    uint8_t top;

    for (i = 0; i < size; i++)
        le[i] = (uint8_t)(v >> (8 * i));

    nbytes = size;
    while (nbytes > min && le[nbytes - 1] == 0)
        nbytes--;

    extra = nbytes - min;          /* bytes beyond min if top byte is folded in */
    room = 7 - extra;              /* value bits left in the lead byte */
    top = le[nbytes - 1];

    if (top >= (1u << room)) {
        /* Top byte doesn't fit beside the prefix: send all nbytes after a
         * lead byte that holds only the prefix (extra + 1 one-bits). */
        out[0] = (uint8_t)(0xFF << room);
        memcpy(out + 1, le, nbytes);
        return (size_t)nbytes + 1;
    }
    out[0] = (uint8_t)(top | (uint8_t)(0xFF << (room + 1)));
    memcpy(out + 1, le, nbytes - 1);
    return (size_t)nbytes;
}

/* Number of extra bytes announced by a lead byte (its leading one-bits,
 * capped at 6 exactly as the reference implementation does). */
static int lead_extra(uint8_t lead)
{
    int n = 0;

    while (n < 6 && (lead & (0x80 >> n)))
        n++;
    return n;
}

static uint64_t decode_var(ars_reader *r, int size, int min)
{
    uint8_t lead, le[9];
    uint64_t v = 0;
    int extra, i;

    memset(le, 0, sizeof le);
    lead = r_byte(r);
    r_bytes(r, le, (size_t)min - 1);
    extra = lead_extra(lead);
    if (extra) {
        if (min + extra > size + 1) {
            if (!r->err)
                r->err = WIRE_EPROTO;
            return 0;
        }
        r_bytes(r, le + min - 1, (size_t)extra);
        le[min - 1 + extra] = lead & (uint8_t)((1u << (8 - extra)) - 1);
    } else {
        le[min - 1] = lead;
    }
    for (i = 0; i < size; i++)
        v |= (uint64_t)le[i] << (8 * i);
    return r->err ? 0 : v;
}

int32_t r_varint(ars_reader *r)
{
    return (int32_t)(uint32_t)decode_var(r, 4, 1);
}

void w_varint(ars_writer *w, int32_t v)
{
    uint8_t b[9];

    w_bytes(w, b, encode_var(b, (uint32_t)v, 4, 1));
}

int64_t r_varlong(ars_reader *r, int min_bytes)
{
    return (int64_t)decode_var(r, 8, min_bytes);
}

void w_varlong(ars_writer *w, int64_t v, int min_bytes)
{
    uint8_t b[9];

    w_bytes(w, b, encode_var(b, (uint64_t)v, 8, min_bytes));
}

/* ------------------------------------------------------------------ */
/* Strings with a 1- or 2-byte length prefix (max 0x7FFF)               */

int r_vstring(ars_reader *r, char *buf, size_t bufsize)
{
    size_t len = r_byte(r);

    if (len & 0x80)
        len = (len & 0x7F) << 8 | r_byte(r);
    if (r->err)
        return -1;
    if (len >= bufsize) {
        r->err = WIRE_EPROTO;
        return -1;
    }
    r_bytes(r, buf, len);
    buf[r->err ? 0 : len] = '\0';
    return r->err ? -1 : (int)len;
}

void w_vstring(ars_writer *w, const char *s, size_t len)
{
    if (len > 0x7FFF) {
        if (!w->err)
            w->err = WIRE_EPROTO;
        return;
    }
    if (len > 0x7F)
        w_byte(w, (uint8_t)(len >> 8 | 0x80));
    w_byte(w, (uint8_t)len);
    w_bytes(w, s, len);
}

/* ------------------------------------------------------------------ */
/* File-list indexes                                                   */
/*
 * NDX_DONE is a single 0 byte. Other negative indexes get a 0xFF prefix and
 * are then sent as their magnitude. Each sign keeps its own "previous"
 * value; a difference of 1..253 is one byte, 0 or 254..32767 is 0xFE plus a
 * big-endian 16-bit diff, anything else is 0xFE plus the absolute value as
 * four bytes with the high bit of the first set.
 */

void ndx_state_init(ars_ndx_state *st)
{
    st->prev_positive = -1;
    st->prev_negative = 1;
}

void w_ndx(ars_writer *w, ars_ndx_state *st, int32_t ndx)
{
    uint8_t b[6];
    size_t n = 0;
    int32_t diff;

    if (ndx == NDX_DONE) {
        w_byte(w, 0);
        return;
    }
    if (ndx >= 0) {
        diff = ndx - st->prev_positive;
        st->prev_positive = ndx;
    } else {
        b[n++] = 0xFF;
        ndx = -ndx;
        diff = ndx - st->prev_negative;
        st->prev_negative = ndx;
    }

    if (diff > 0 && diff < 0xFE) {
        b[n++] = (uint8_t)diff;
    } else if (diff < 0 || diff > 0x7FFF) {
        b[n++] = 0xFE;
        b[n++] = (uint8_t)((uint32_t)ndx >> 24 | 0x80);
        b[n++] = (uint8_t)ndx;
        b[n++] = (uint8_t)((uint32_t)ndx >> 8);
        b[n++] = (uint8_t)((uint32_t)ndx >> 16);
    } else {
        b[n++] = 0xFE;
        b[n++] = (uint8_t)(diff >> 8);
        b[n++] = (uint8_t)diff;
    }
    w_bytes(w, b, n);
}

int32_t r_ndx(ars_reader *r, ars_ndx_state *st)
{
    uint8_t b, x[4];
    int32_t *prev, num;
    int negative = 0;

    b = r_byte(r);
    if (r->err)
        return 0;
    if (b == 0)
        return NDX_DONE;
    if (b == 0xFF) {
        negative = 1;
        b = r_byte(r);
    }
    prev = negative ? &st->prev_negative : &st->prev_positive;

    if (b == 0xFE) {
        r_bytes(r, x, 2);
        if (x[0] & 0x80) {
            r_bytes(r, x + 2, 2);
            num = (int32_t)((uint32_t)(x[0] & 0x7F) << 24 | (uint32_t)x[3] << 16
                            | (uint32_t)x[2] << 8 | x[1]);
        } else {
            num = (x[0] << 8 | x[1]) + *prev;
        }
    } else {
        num = b + *prev;
    }
    if (r->err)
        return 0;
    *prev = num;
    return negative ? -num : num;
}

int32_t r_ndx_skip_stats(ars_reader *r, ars_ndx_state *st, int32_t *del)
{
    int32_t ndx, v;
    int i;

    while ((ndx = r_ndx(r, st)) == NDX_DEL_STATS && !r->err) {
        for (i = 0; i < 5; i++) {
            v = r_varint(r);
            if (del)
                del[i] = v;
        }
    }
    return ndx;
}

/* ------------------------------------------------------------------ */
/* Memory buffers                                                      */

void membuf_init(ars_membuf *mb)
{
    memset(mb, 0, sizeof *mb);
}

void membuf_free(ars_membuf *mb)
{
    free(mb->data);
    membuf_init(mb);
}

static int membuf_read(void *ctx, uint8_t *dst, size_t n)
{
    ars_membuf *mb = ctx;

    if (mb->len - mb->pos < n)
        return WIRE_EOF;
    memcpy(dst, mb->data + mb->pos, n);
    mb->pos += n;
    return WIRE_OK;
}

static int membuf_write(void *ctx, const uint8_t *src, size_t n)
{
    ars_membuf *mb = ctx;

    if (mb->cap - mb->len < n) {
        size_t cap = mb->cap ? mb->cap : 256;
        uint8_t *p;

        while (cap - mb->len < n)
            cap *= 2;
        p = realloc(mb->data, cap);
        if (!p)
            return WIRE_ENOMEM;
        mb->data = p;
        mb->cap = cap;
    }
    memcpy(mb->data + mb->len, src, n);
    mb->len += n;
    return WIRE_OK;
}

void membuf_reader(ars_reader *r, ars_membuf *mb)
{
    r->read = membuf_read;
    r->ctx = mb;
    r->err = WIRE_OK;
}

void membuf_writer(ars_writer *w, ars_membuf *mb)
{
    w->write = membuf_write;
    w->ctx = mb;
    w->err = WIRE_OK;
}

/* ------------------------------------------------------------------ */
/* Multiplexing                                                        */
/*
 * Each frame starts with a little-endian 32-bit header: the top byte is
 * MPLEX_BASE + message tag, the low 24 bits the payload length.
 */

void mplex_encode_header(uint8_t out[4], int tag, size_t len)
{
    put_le32(out, (uint32_t)(MPLEX_BASE + tag) << 24 | (uint32_t)(len & MPLEX_MAX_LEN));
}

int mplex_decode_header(const uint8_t in[4], int *tag, size_t *len)
{
    uint32_t h = get_le32(in);

    if ((h >> 24) < MPLEX_BASE)
        return WIRE_EPROTO;
    *tag = (int)(h >> 24) - MPLEX_BASE;
    *len = h & MPLEX_MAX_LEN;
    return WIRE_OK;
}

void demux_init(ars_demux *d, ars_reader *raw, ars_msg_fn on_msg, void *msg_ctx)
{
    d->raw = raw;
    d->data_left = 0;
    d->on_msg = on_msg;
    d->msg_ctx = msg_ctx;
    d->msgbuf = NULL;
}

void demux_free(ars_demux *d)
{
    free(d->msgbuf);
    d->msgbuf = NULL;
}

int demux_fill(ars_demux *d)
{
    uint8_t hdr[4];
    size_t len;
    int tag, rc;

    while (d->data_left == 0) {
        r_bytes(d->raw, hdr, 4);
        if (d->raw->err)
            return d->raw->err;
        if (mplex_decode_header(hdr, &tag, &len) != WIRE_OK)
            return WIRE_EPROTO;
        if (tag == MSG_DATA) {
            d->data_left = len;
            continue;
        }

        if (len > MPLEX_MAX_MSG)
            return WIRE_EPROTO;
        if (!d->msgbuf && !(d->msgbuf = malloc(MPLEX_MAX_MSG)))
            return WIRE_ENOMEM;
        r_bytes(d->raw, d->msgbuf, len);
        if (d->raw->err)
            return d->raw->err;
        if (d->on_msg) {
            rc = d->on_msg(d->msg_ctx, tag, d->msgbuf, len);
            if (rc != WIRE_OK)
                return rc;
        }
    }
    return WIRE_OK;
}

static int demux_read(void *ctx, uint8_t *dst, size_t n)
{
    ars_demux *d = ctx;
    size_t chunk;
    int rc;

    while (n > 0) {
        if ((rc = demux_fill(d)) != WIRE_OK)
            return rc;
        chunk = n < d->data_left ? n : d->data_left;
        r_bytes(d->raw, dst, chunk);
        if (d->raw->err)
            return d->raw->err;
        d->data_left -= chunk;
        dst += chunk;
        n -= chunk;
    }
    return WIRE_OK;
}

void demux_reader(ars_reader *r, ars_demux *d)
{
    r->read = demux_read;
    r->ctx = d;
    r->err = WIRE_OK;
}

int mux_init(ars_mux *m, ars_writer *raw)
{
    m->raw = raw;
    m->len = 0;
    m->buf = malloc(MUX_BUFSIZE);
    return m->buf ? WIRE_OK : WIRE_ENOMEM;
}

void mux_free(ars_mux *m)
{
    free(m->buf);
    m->buf = NULL;
}

static int mux_frame(ars_mux *m, int tag, const void *payload, size_t len)
{
    uint8_t hdr[4];

    mplex_encode_header(hdr, tag, len);
    w_bytes(m->raw, hdr, 4);
    w_bytes(m->raw, payload, len);
    return m->raw->err;
}

int mux_flush(ars_mux *m)
{
    int rc;

    if (m->len == 0)
        return m->raw->err;
    rc = mux_frame(m, MSG_DATA, m->buf, m->len);
    m->len = 0;
    return rc;
}

static int mux_write(void *ctx, const uint8_t *src, size_t n)
{
    ars_mux *m = ctx;
    size_t chunk;
    int rc;

    while (n > 0) {
        chunk = MUX_BUFSIZE - m->len;
        if (chunk > n)
            chunk = n;
        memcpy(m->buf + m->len, src, chunk);
        m->len += chunk;
        src += chunk;
        n -= chunk;
        if (m->len == MUX_BUFSIZE && (rc = mux_flush(m)) != WIRE_OK)
            return rc;
    }
    return WIRE_OK;
}

void mux_writer(ars_writer *w, ars_mux *m)
{
    w->write = mux_write;
    w->ctx = m;
    w->err = WIRE_OK;
}

int mux_send_msg(ars_mux *m, int tag, const void *payload, size_t len)
{
    int rc;

    if (len > MPLEX_MAX_LEN)
        return WIRE_EPROTO;
    if ((rc = mux_flush(m)) != WIRE_OK)
        return rc;
    return mux_frame(m, tag, payload, len);
}
