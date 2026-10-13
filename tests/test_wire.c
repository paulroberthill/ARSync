/* test_wire.c - wire encodings and multiplexing */
#include <stdlib.h>

#include "test.h"
#include "wire.h"
#include "vectors.h"

#define COUNT(a) (sizeof(a) / sizeof((a)[0]))

/* Encoders must produce the reference bytes; decoders must read them back. */
static void test_reference_vectors(void)
{
    ars_membuf mb;
    ars_reader r;
    ars_writer w;
    ars_ndx_state ws, rs;
    size_t i;

    for (i = 0; i < COUNT(varint_vec); i++) {
        membuf_init(&mb);
        membuf_writer(&w, &mb);
        w_varint(&w, varint_vec[i].v);
        CHECK_EQ(mb.len, varint_vec[i].len);
        CHECK_MEM(mb.data, varint_vec[i].b, varint_vec[i].len);
        membuf_reader(&r, &mb);
        CHECK_EQ(r_varint(&r), varint_vec[i].v);
        CHECK_EQ(r.err, WIRE_OK);
        CHECK_EQ(mb.pos, mb.len);
        membuf_free(&mb);
    }

    for (i = 0; i < COUNT(varlong_vec); i++) {
        membuf_init(&mb);
        membuf_writer(&w, &mb);
        w_varlong(&w, varlong_vec[i].v, varlong_vec[i].min);
        CHECK_EQ(mb.len, varlong_vec[i].len);
        CHECK_MEM(mb.data, varlong_vec[i].b, varlong_vec[i].len);
        membuf_reader(&r, &mb);
        CHECK_EQ(r_varlong(&r, varlong_vec[i].min), varlong_vec[i].v);
        CHECK_EQ(r.err, WIRE_OK);
        CHECK_EQ(mb.pos, mb.len);
        membuf_free(&mb);
    }

    /* Indexes are delta-encoded, so encode and decode the whole sequence
     * with one state per direction. */
    ndx_state_init(&ws);
    ndx_state_init(&rs);
    for (i = 0; i < COUNT(ndx_vec); i++) {
        membuf_init(&mb);
        membuf_writer(&w, &mb);
        w_ndx(&w, &ws, ndx_vec[i].v);
        CHECK_EQ(mb.len, ndx_vec[i].len);
        CHECK_MEM(mb.data, ndx_vec[i].b, ndx_vec[i].len);
        membuf_reader(&r, &mb);
        CHECK_EQ(r_ndx(&r, &rs), ndx_vec[i].v);
        CHECK_EQ(r.err, WIRE_OK);
        membuf_free(&mb);
    }
}

/* Deletion counts from a receiving server (NDX_DEL_STATS and five
 * varints) are skipped wherever a request, reply or goodbye is read. */
static void test_ndx_skip_stats(void)
{
    static const uint8_t first_del_stats[] = { 0xFF, 0x02 };   /* -3 after -1 */
    ars_membuf mb;
    ars_reader r;
    ars_writer w;
    ars_ndx_state ws, rs;
    int i;

    membuf_init(&mb);
    membuf_writer(&w, &mb);
    ndx_state_init(&ws);
    w_ndx(&w, &ws, NDX_DEL_STATS);
    CHECK_EQ(mb.len, sizeof first_del_stats);
    CHECK_MEM(mb.data, first_del_stats, sizeof first_del_stats);
    for (i = 0; i < 5; i++)
        w_varint(&w, i == 0 ? 123456 : i);
    w_ndx(&w, &ws, 7);
    w_ndx(&w, &ws, NDX_DEL_STATS);
    for (i = 0; i < 5; i++)
        w_varint(&w, 0);
    w_ndx(&w, &ws, NDX_DEL_STATS);
    for (i = 0; i < 5; i++)
        w_varint(&w, 1);
    w_ndx(&w, &ws, NDX_DONE);

    membuf_reader(&r, &mb);
    ndx_state_init(&rs);
    CHECK_EQ(r_ndx_skip_stats(&r, &rs), 7);
    CHECK_EQ(r_ndx_skip_stats(&r, &rs), NDX_DONE);
    CHECK_EQ(r.err, WIRE_OK);
    CHECK_EQ(mb.pos, mb.len);

    /* Cut off inside the counts: an error, not a loop. */
    mb.pos = 0;
    mb.len = 4;
    membuf_reader(&r, &mb);
    ndx_state_init(&rs);
    r_ndx_skip_stats(&r, &rs);
    CHECK(r.err != WIRE_OK);
    membuf_free(&mb);
}

static void test_fixed_values(void)
{
    static const uint8_t int_bytes[] = { 0x78, 0x56, 0x34, 0x12 };
    static const uint8_t long_bytes[] = {
        0xff, 0xff, 0xff, 0xff, 0x89, 0x67, 0x45, 0x23, 0x01, 0x00, 0x00, 0x00
    };
    ars_membuf mb;
    ars_reader r;
    ars_writer w;

    membuf_init(&mb);
    membuf_writer(&w, &mb);
    w_int32(&w, 0x12345678);
    w_longint(&w, 0x123456789LL);
    w_longint(&w, 1000);
    w_longint(&w, -1);
    CHECK_EQ(mb.len, 4 + 12 + 4 + 12);
    CHECK_MEM(mb.data, int_bytes, 4);
    CHECK_MEM(mb.data + 4, long_bytes, 12);

    membuf_reader(&r, &mb);
    CHECK_EQ(r_int32(&r), 0x12345678);
    CHECK_EQ(r_longint(&r), 0x123456789LL);
    CHECK_EQ(r_longint(&r), 1000);
    CHECK_EQ(r_longint(&r), -1);
    CHECK_EQ(r.err, WIRE_OK);
    membuf_free(&mb);
}

static void test_vstrings(void)
{
    char big[300], buf[400];
    ars_membuf mb;
    ars_reader r;
    ars_writer w;

    memset(big, 'x', sizeof big);
    membuf_init(&mb);
    membuf_writer(&w, &mb);
    w_vstring(&w, "md5 md4", 7);
    w_vstring(&w, big, sizeof big);
    w_vstring(&w, "", 0);
    CHECK_EQ(mb.data[0], 7);
    CHECK_EQ(mb.data[8], 0x80 | (300 >> 8));
    CHECK_EQ(mb.data[9], 300 & 0xff);

    membuf_reader(&r, &mb);
    CHECK_EQ(r_vstring(&r, buf, sizeof buf), 7);
    CHECK(strcmp(buf, "md5 md4") == 0);
    CHECK_EQ(r_vstring(&r, buf, sizeof buf), 300);
    CHECK_MEM(buf, big, 300);
    CHECK_EQ(r_vstring(&r, buf, sizeof buf), 0);
    CHECK_EQ(r.err, WIRE_OK);

    /* A string longer than the caller's buffer is a protocol error. */
    mb.pos = 0;
    membuf_reader(&r, &mb);
    CHECK_EQ(r_vstring(&r, buf, 7), -1);
    CHECK_EQ(r.err, WIRE_EPROTO);

    /* Over-long strings can't be encoded. */
    membuf_writer(&w, &mb);
    w_vstring(&w, big, 0x8000);
    CHECK_EQ(w.err, WIRE_EPROTO);
    membuf_free(&mb);
}

static void test_sticky_errors(void)
{
    static const uint8_t truncated[] = { 0xe0, 0x00 };   /* varint wanting 3 more bytes */
    static const uint8_t overlong[] = { 0xfc, 0, 0, 0, 0, 0, 0 };  /* varint: 6 extra bytes */
    ars_membuf mb;
    ars_reader r;

    membuf_init(&mb);
    mb.data = (uint8_t *)truncated;
    mb.len = sizeof truncated;
    membuf_reader(&r, &mb);
    CHECK_EQ(r_varint(&r), 0);
    CHECK_EQ(r.err, WIRE_EOF);
    /* Once failed, later reads return 0 without touching the source. */
    CHECK_EQ(r_int32(&r), 0);
    CHECK_EQ(r.err, WIRE_EOF);

    membuf_init(&mb);
    mb.data = (uint8_t *)overlong;
    mb.len = sizeof overlong;
    membuf_reader(&r, &mb);
    CHECK_EQ(r_varint(&r), 0);
    CHECK_EQ(r.err, WIRE_EPROTO);
}

/* --- multiplexing --- */

struct msg_log {
    int count;
    int tag;
    char text[64];
    int abort_on;
};

static int log_msg(void *ctx, int tag, const uint8_t *payload, size_t len)
{
    struct msg_log *log = ctx;

    log->count++;
    log->tag = tag;
    if (len >= sizeof log->text)
        len = sizeof log->text - 1;
    memcpy(log->text, payload, len);
    log->text[len] = '\0';
    return tag == log->abort_on ? WIRE_EABORT : WIRE_OK;
}

static void put_frame(ars_writer *w, int tag, const char *payload, size_t len)
{
    uint8_t hdr[4];

    mplex_encode_header(hdr, tag, len);
    w_bytes(w, hdr, 4);
    w_bytes(w, payload, len);
}

static void test_mplex_header(void)
{
    static const uint8_t data5[] = { 0x05, 0x00, 0x00, 0x07 };
    static const uint8_t info_max[] = { 0xff, 0xff, 0xff, 0x09 };
    static const uint8_t bad[] = { 0x00, 0x00, 0x00, 0x06 };
    uint8_t hdr[4];
    size_t len;
    int tag;

    mplex_encode_header(hdr, MSG_DATA, 5);
    CHECK_MEM(hdr, data5, 4);
    mplex_encode_header(hdr, MSG_INFO, MPLEX_MAX_LEN);
    CHECK_MEM(hdr, info_max, 4);

    CHECK_EQ(mplex_decode_header(info_max, &tag, &len), WIRE_OK);
    CHECK_EQ(tag, MSG_INFO);
    CHECK_EQ(len, MPLEX_MAX_LEN);
    CHECK_EQ(mplex_decode_header(bad, &tag, &len), WIRE_EPROTO);
}

static void test_demux(void)
{
    struct msg_log log;
    ars_membuf raw;
    ars_reader rr, r;
    ars_writer rw;
    ars_demux d;
    char buf[16];

    /* Data split across frames, with a message and an empty frame between. */
    membuf_init(&raw);
    membuf_writer(&rw, &raw);
    put_frame(&rw, MSG_DATA, "abc", 3);
    put_frame(&rw, MSG_INFO, "hello", 5);
    put_frame(&rw, MSG_DATA, "", 0);
    put_frame(&rw, MSG_DATA, "defg", 4);

    memset(&log, 0, sizeof log);
    log.abort_on = -1;
    membuf_reader(&rr, &raw);
    demux_init(&d, &rr, log_msg, &log);
    demux_reader(&r, &d);
    r_bytes(&r, buf, 6);
    CHECK_EQ(r.err, WIRE_OK);
    CHECK_MEM(buf, "abcdef", 6);
    CHECK_EQ(log.count, 1);
    CHECK_EQ(log.tag, MSG_INFO);
    CHECK(strcmp(log.text, "hello") == 0);
    CHECK_EQ(d.data_left, 1);
    CHECK_EQ(r_byte(&r), 'g');
    /* The stream then ends cleanly between frames. */
    r_byte(&r);
    CHECK_EQ(r.err, WIRE_EOF);
    demux_free(&d);

    /* A handler can abort the read. */
    memset(&log, 0, sizeof log);
    log.abort_on = MSG_ERROR;
    raw.len = raw.pos = 0;
    put_frame(&rw, MSG_ERROR, "boom", 4);
    put_frame(&rw, MSG_DATA, "x", 1);
    membuf_reader(&rr, &raw);
    demux_init(&d, &rr, log_msg, &log);
    demux_reader(&r, &d);
    r_byte(&r);
    CHECK_EQ(r.err, WIRE_EABORT);
    CHECK(strcmp(log.text, "boom") == 0);
    demux_free(&d);

    /* Messages larger than we're willing to buffer are rejected. */
    raw.len = raw.pos = 0;
    {
        uint8_t hdr[4];

        mplex_encode_header(hdr, MSG_INFO, MPLEX_MAX_MSG + 1);
        w_bytes(&rw, hdr, 4);
    }
    membuf_reader(&rr, &raw);
    demux_init(&d, &rr, NULL, NULL);
    demux_reader(&r, &d);
    r_byte(&r);
    CHECK_EQ(r.err, WIRE_EPROTO);
    demux_free(&d);

    membuf_free(&raw);
}

static void test_mux(void)
{
    enum { TOTAL = MUX_BUFSIZE + 1000 };
    ars_membuf raw;
    ars_writer rw, w;
    ars_mux m;
    uint8_t *data;
    size_t len;
    int tag, i;

    data = malloc(TOTAL);
    CHECK(data != NULL);
    if (!data)
        return;
    for (i = 0; i < TOTAL; i++)
        data[i] = (uint8_t)(i * 7);

    membuf_init(&raw);
    membuf_writer(&rw, &raw);
    CHECK_EQ(mux_init(&m, &rw), WIRE_OK);
    mux_writer(&w, &m);

    /* A full buffer is framed immediately; the rest waits for a flush. */
    w_bytes(&w, data, TOTAL);
    CHECK_EQ(w.err, WIRE_OK);
    CHECK_EQ(raw.len, 4 + MUX_BUFSIZE);
    /* Sending a message flushes pending data ahead of it. */
    CHECK_EQ(mux_send_msg(&m, MSG_INFO, "hi", 2), WIRE_OK);
    CHECK_EQ(mux_flush(&m), WIRE_OK);   /* nothing pending: no empty frame */
    CHECK_EQ(raw.len, 4 + MUX_BUFSIZE + 4 + 1000 + 4 + 2);

    CHECK_EQ(mplex_decode_header(raw.data, &tag, &len), WIRE_OK);
    CHECK_EQ(tag, MSG_DATA);
    CHECK_EQ(len, MUX_BUFSIZE);
    CHECK_MEM(raw.data + 4, data, MUX_BUFSIZE);
    CHECK_EQ(mplex_decode_header(raw.data + 4 + MUX_BUFSIZE, &tag, &len), WIRE_OK);
    CHECK_EQ(tag, MSG_DATA);
    CHECK_EQ(len, 1000);
    CHECK_MEM(raw.data + 8 + MUX_BUFSIZE, data + MUX_BUFSIZE, 1000);
    CHECK_EQ(mplex_decode_header(raw.data + 8 + MUX_BUFSIZE + 1000, &tag, &len), WIRE_OK);
    CHECK_EQ(tag, MSG_INFO);
    CHECK_EQ(len, 2);

    mux_free(&m);
    membuf_free(&raw);
    free(data);
}

void test_wire(void)
{
    test_reference_vectors();
    test_ndx_skip_stats();
    test_fixed_values();
    test_vstrings();
    test_sticky_errors();
    test_mplex_header();
    test_demux();
    test_mux();
}
