/* test_pull.c - a whole pull against a scripted server stream, covering
 * what a real daemon won't do on demand: a file that fails verification
 * and is fetched again in the redo phase. Runs on the host and under vamos.
 */
#include <stdlib.h>

#include "test.h"
#include "client.h"
#include "digest.h"
#include "flist.h"
#include "plat.h"
#include "pull.h"
#include "checksum.h"

#define DEST "arsync-test.pull"

static const char content[] = "hello from the redo phase\n";

static void put_file_reply(ars_writer *w, ars_ndx_state *st, int32_t ndx, int corrupt)
{
    uint8_t sum[DIGEST_LEN];
    ars_digest d;

    w_ndx(w, st, ndx);
    w_uint16(w, ITEM_TRANSFER | ITEM_IS_NEW);
    w_int32(w, 0);                       /* checksum header, as we sent it */
    w_int32(w, 0);
    w_int32(w, 0);
    w_int32(w, 0);
    w_int32(w, (int32_t)strlen(content)); /* one literal token */
    w_bytes(w, content, strlen(content));
    w_int32(w, 0);                       /* end of tokens */
    digest_init(&d, DIGEST_MD5);
    digest_update(&d, content, strlen(content));
    digest_final(&d, sum);
    if (corrupt)
        sum[0] ^= 0xFF;
    w_bytes(w, sum, sizeof sum);
}

/* What a protocol-32 sender sends for ".", "file", with "file" failing
 * verification the first time. */
static void build_server_stream(ars_membuf *mb)
{
    ars_writer raw, w;
    ars_ndx_state st;
    ars_mux mux;

    membuf_writer(&raw, mb);
    mux_init(&mux, &raw);
    mux_writer(&w, &mux);
    ndx_state_init(&st);

    /* File list. */
    w_varint(&w, XMIT_TOP_DIR);
    w_byte(&w, 1);
    w_bytes(&w, ".", 1);
    w_varlong(&w, 4096, 3);
    w_varlong(&w, 1700000000, 4);
    w_int32(&w, WIRE_S_IFDIR | 0755);
    w_varint(&w, XMIT_SAME_TIME);
    w_byte(&w, 4);
    w_bytes(&w, "file", 4);
    w_varlong(&w, (int64_t)strlen(content), 3);
    w_int32(&w, WIRE_S_IFREG | 0644);
    w_varint(&w, 0);
    w_varint(&w, 0);

    /* Phase 1: "." was created locally (echoed back), "file" arrives bad. */
    w_ndx(&w, &st, 0);
    w_uint16(&w, ITEM_LOCAL_CHANGE | ITEM_IS_NEW);
    put_file_reply(&w, &st, 1, 1);
    w_ndx(&w, &st, NDX_DONE);
    /* Phase 2: the redo, intact this time. */
    put_file_reply(&w, &st, 1, 0);
    w_ndx(&w, &st, NDX_DONE);
    w_ndx(&w, &st, NDX_DONE);
    w_varlong(&w, 100, 3);               /* statistics */
    w_varlong(&w, 200, 3);
    w_varlong(&w, (int64_t)strlen(content), 3);
    w_varlong(&w, 0, 3);
    w_varlong(&w, 0, 3);
    w_ndx(&w, &st, NDX_DONE);            /* goodbye echo */
    mux_flush(&mux);
    mux_free(&mux);
}

/* Decodes our requests and checks that "file" was asked for twice. */
static void check_requests(ars_membuf *sent)
{
    ars_reader raw, r;
    ars_demux d;
    ars_ndx_state st;
    int32_t ndx;
    int file_requests = 0, dones = 0;
    uint16_t iflags;

    membuf_reader(&raw, sent);
    demux_init(&d, &raw, NULL, NULL);
    demux_reader(&r, &d);
    ndx_state_init(&st);
    while (!r.err) {
        ndx = r_ndx(&r, &st);
        if (r.err)
            break;
        if (ndx == NDX_DONE) {
            dones++;
            continue;
        }
        iflags = r_uint16(&r);
        if (iflags & ITEM_TRANSFER) {
            CHECK_EQ(ndx, 1);
            file_requests++;
            r_int32(&r);
            r_int32(&r);
            r_int32(&r);
            r_int32(&r);
        }
    }
    CHECK_EQ(file_requests, 2);
    CHECK_EQ(dones, 5);     /* end of phase 1, three for phase 2, goodbye */
    demux_free(&d);
}

/* A delta reply: the local file holds 700 x 'A' then 700 x 'B' (two
 * blocks); the new contents are block 1, "new", block 0. */
static void build_delta_stream(ars_membuf *mb, uint8_t *expect, size_t *expect_len)
{
    ars_writer raw, w;
    ars_ndx_state st;
    ars_mux mux;
    uint8_t sum[DIGEST_LEN];
    ars_digest d;
    size_t n = 0;

    memset(expect, 'B', 700);
    memcpy(expect + 700, "new", 3);
    memset(expect + 703, 'A', 700);
    n = 1403;
    *expect_len = n;

    membuf_writer(&raw, mb);
    mux_init(&mux, &raw);
    mux_writer(&w, &mux);
    ndx_state_init(&st);

    w_varint(&w, XMIT_TOP_DIR);          /* file list: ".", "file" */
    w_byte(&w, 1);
    w_bytes(&w, ".", 1);
    w_varlong(&w, 4096, 3);
    w_varlong(&w, 1700000000, 4);
    w_int32(&w, WIRE_S_IFDIR | 0755);
    w_varint(&w, XMIT_SAME_TIME);
    w_byte(&w, 4);
    w_bytes(&w, "file", 4);
    w_varlong(&w, (int64_t)n, 3);
    w_int32(&w, WIRE_S_IFREG | 0644);
    w_varint(&w, 0);
    w_varint(&w, 0);

    w_ndx(&w, &st, 1);                   /* "." exists already: no echo */
    w_uint16(&w, ITEM_TRANSFER | ITEM_REPORT_SIZE | ITEM_REPORT_TIME);
    w_int32(&w, 2);                      /* the checksum header we sent */
    w_int32(&w, 700);
    w_int32(&w, 2);
    w_int32(&w, 0);
    w_int32(&w, -2);                     /* block 1 */
    w_int32(&w, 3);
    w_bytes(&w, "new", 3);
    w_int32(&w, -1);                     /* block 0 */
    w_int32(&w, 0);
    digest_init(&d, DIGEST_MD5);
    digest_update(&d, expect, n);
    digest_final(&d, sum);
    w_bytes(&w, sum, sizeof sum);
    w_ndx(&w, &st, NDX_DONE);
    w_ndx(&w, &st, NDX_DONE);
    w_ndx(&w, &st, NDX_DONE);
    w_varlong(&w, 1, 3);
    w_varlong(&w, 1, 3);
    w_varlong(&w, (int64_t)n, 3);
    w_varlong(&w, 0, 3);
    w_varlong(&w, 0, 3);
    w_ndx(&w, &st, NDX_DONE);
    mux_flush(&mux);
    mux_free(&mux);
}

static void setup_session(ars_session *s, ars_membuf *from, ars_membuf *to,
                          ars_reader *raw_in, ars_writer *raw_out)
{
    memset(s, 0, sizeof *s);
    s->protocol = 32;
    s->compat_flags = CF_VARINT_FLIST_FLAGS | CF_SAFE_FLIST | CF_CHKSUM_SEED_FIX;
    s->checksum_type = DIGEST_MD5;
    s->checksum_seed = 12345;
    s->remote_exit = -1;
    membuf_reader(raw_in, from);
    membuf_writer(raw_out, to);
    demux_init(&s->demux, raw_in, NULL, s);
    demux_reader(&s->in, &s->demux);
    mux_init(&s->mux, raw_out);
    mux_writer(&s->out, &s->mux);
    s->multiplexing = 1;
}

static void test_delta(void)
{
    ars_membuf from_server, to_server;
    ars_reader raw_in, r;
    ars_writer raw_out;
    ars_session s;
    ars_options o;
    ars_demux dm;
    ars_ndx_state st;
    plat_file *f;
    uint8_t old[1400], expect[1500], got[1500];
    size_t expect_len;
    long n;

    plat_remove(DEST "/file");
    plat_remove(DEST);
    CHECK_EQ(plat_mkdir(DEST), PLAT_OK);
    memset(old, 'A', 700);
    memset(old + 700, 'B', 700);
    CHECK_EQ(plat_open_write(DEST "/file", &f), PLAT_OK);
    CHECK_EQ(plat_write(f, old, sizeof old), PLAT_OK);
    plat_close(f);

    membuf_init(&from_server);
    membuf_init(&to_server);
    build_delta_stream(&from_server, expect, &expect_len);
    setup_session(&s, &from_server, &to_server, &raw_in, &raw_out);
    memset(&o, 0, sizeof o);
    o.recursive = 1;

    CHECK_EQ(client_pull(&s, &o, DEST), RERR_OK);
    CHECK_EQ(from_server.pos, from_server.len);

    /* Our request carried the two blocks' checksums. */
    {
        int32_t ndx;
        uint16_t iflags;
        uint8_t strong[SUM_LENGTH], sent[2];

        membuf_reader(&raw_in, &to_server);
        demux_init(&dm, &raw_in, NULL, NULL);
        demux_reader(&r, &dm);
        ndx_state_init(&st);
        ndx = r_ndx(&r, &st);
        iflags = r_uint16(&r);
        CHECK_EQ(ndx, 1);
        CHECK(iflags & ITEM_TRANSFER);
        CHECK_EQ(r_int32(&r), 2);       /* count */
        CHECK_EQ(r_int32(&r), 700);     /* block length */
        CHECK_EQ(r_int32(&r), 2);       /* s2length */
        CHECK_EQ(r_int32(&r), 0);       /* remainder */
        CHECK_EQ(r_int32(&r), (int32_t)rolling_sum(old, 700));
        r_bytes(&r, sent, 2);
        block_sum(DIGEST_MD5, 12345, 1, old, 700, strong);
        CHECK_MEM(sent, strong, 2);
        CHECK_EQ(r_int32(&r), (int32_t)rolling_sum(old + 700, 700));
        r_bytes(&r, sent, 2);
        block_sum(DIGEST_MD5, 12345, 1, old + 700, 700, strong);
        CHECK_MEM(sent, strong, 2);
        CHECK_EQ(r.err, WIRE_OK);
        demux_free(&dm);
    }

    CHECK_EQ(plat_open_read(DEST "/file", &f), PLAT_OK);
    n = plat_read(f, got, sizeof got);
    plat_close(f);
    CHECK_EQ(n, (long)expect_len);
    CHECK(n > 0 && memcmp(got, expect, (size_t)n) == 0);

    demux_free(&s.demux);
    mux_free(&s.mux);
    membuf_free(&from_server);
    membuf_free(&to_server);
    CHECK_EQ(plat_remove(DEST "/file"), PLAT_OK);
    CHECK_EQ(plat_remove(DEST), PLAT_OK);
}

void test_pull_suite(void)
{
    ars_membuf from_server, to_server;
    ars_reader raw_in;
    ars_writer raw_out;
    ars_session s;
    ars_options o;
    plat_fileinfo st;
    plat_file *f;
    char buf[64];
    long n;

    plat_remove(DEST "/file");
    plat_remove(DEST);

    membuf_init(&from_server);
    membuf_init(&to_server);
    build_server_stream(&from_server);

    memset(&s, 0, sizeof s);
    s.protocol = 32;
    s.compat_flags = CF_VARINT_FLIST_FLAGS | CF_SAFE_FLIST;
    s.checksum_type = DIGEST_MD5;
    s.remote_exit = -1;
    membuf_reader(&raw_in, &from_server);
    membuf_writer(&raw_out, &to_server);
    demux_init(&s.demux, &raw_in, NULL, &s);
    demux_reader(&s.in, &s.demux);
    mux_init(&s.mux, &raw_out);
    mux_writer(&s.out, &s.mux);
    s.multiplexing = 1;

    memset(&o, 0, sizeof o);
    o.recursive = 1;
    o.times = 1;

    CHECK_EQ(client_pull(&s, &o, DEST), RERR_OK);
    CHECK_EQ(from_server.pos, from_server.len);     /* everything consumed */
    check_requests(&to_server);

    CHECK_EQ(plat_stat(DEST "/file", &st), PLAT_OK);
    CHECK_EQ(st.size, (int64_t)strlen(content));
    CHECK_EQ(st.mtime, 1700000000);
    if (plat_open_read(DEST "/file", &f) == PLAT_OK) {
        n = plat_read(f, buf, sizeof buf);
        CHECK_EQ(n, (long)strlen(content));
        CHECK(n > 0 && memcmp(buf, content, (size_t)n) == 0);
        plat_close(f);
    }
    /* No temp files left behind. */
    {
        plat_dir *d;
        char name[64];
        int entries = 0;

        if (plat_opendir(DEST, &d) == PLAT_OK) {
            while (plat_readdir(d, name, sizeof name, NULL) == 1)
                entries++;
            plat_closedir(d);
        }
        CHECK_EQ(entries, 1);
    }

    demux_free(&s.demux);
    mux_free(&s.mux);
    membuf_free(&from_server);
    membuf_free(&to_server);
    CHECK_EQ(plat_remove(DEST "/file"), PLAT_OK);
    CHECK_EQ(plat_remove(DEST), PLAT_OK);
    test_delta();
}
