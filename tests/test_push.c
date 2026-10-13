/* test_push.c - the sending side: file-list encoding and delta matching */
#include <stdlib.h>

#include "test.h"
#include "checksum.h"
#include "digest.h"
#include "flist.h"
#include "match.h"
#include "options.h"
#include "output.h"
#include "plat.h"
#include "token.h"

/* What a sender encodes, a receiver must decode to the same list; with
 * sums (-c), each regular file's checksum too. */
static void test_flist_roundtrip(int varint, int sums)
{
    char longname[400], err[64];
    ars_flist_format fmt;
    ars_flist a, b;
    ars_membuf mb;
    ars_reader r;
    ars_writer w;
    size_t i;

    memset(longname, 'q', 300);
    memcpy(longname, "dir/", 4);
    longname[300] = '\0';

    flist_init(&a);
    a.sum_len = sums ? DIGEST_LEN : 0;
    flist_add(&a, ".", WIRE_S_IFDIR | 0755, 0, 1700000000, 0);
    a.files[0].xflags = XMIT_TOP_DIR;
    flist_add(&a, "dir", WIRE_S_IFDIR | 0755, 0, 1700000000, 0);
    flist_add(&a, "dir/file-a", WIRE_S_IFREG | 0644, 5000000000LL, 1700000001, 999);
    flist_add(&a, "dir/file-b", WIRE_S_IFREG | 0644, 0, 1700000001, 0);
    flist_add(&a, longname, WIRE_S_IFREG | 0600, 7, -5, 0);
    flist_add(&a, "link", WIRE_S_IFLNK | 0777, 1, 1, 0);
    for (i = 0; i < a.count; i++)
        if (sums && WIRE_ISREG(a.files[i].mode))
            memset(flist_sum(&a, &a.files[i]), (int)(0xA0 + i), DIGEST_LEN);
    flist_sort_and_clean(&a);

    fmt.varint_flags = varint;
    fmt.safe_flist = 1;
    membuf_init(&mb);
    membuf_writer(&w, &mb);
    flist_send(&a, &w, &fmt, 32, IOERR_GENERAL);
    flist_init(&b);
    b.sum_len = a.sum_len;
    membuf_reader(&r, &mb);
    CHECK_EQ(flist_recv(&b, &r, &fmt, err, sizeof err), WIRE_OK);
    CHECK_EQ(mb.pos, mb.len);
    CHECK_EQ(b.io_error, IOERR_GENERAL);
    CHECK_EQ(b.count, a.count);
    for (i = 0; i < a.count && i < b.count; i++) {
        CHECK(strcmp(flist_name(&a, &a.files[i]), flist_name(&b, &b.files[i])) == 0);
        CHECK_EQ(a.files[i].mode, b.files[i].mode);
        CHECK_EQ(a.files[i].size, b.files[i].size);
        CHECK_EQ(a.files[i].mtime, b.files[i].mtime);
        CHECK_EQ(a.files[i].mtime_nsec, b.files[i].mtime_nsec);
        if (sums && WIRE_ISREG(a.files[i].mode))
            CHECK_MEM(flist_sum(&b, &b.files[i]), flist_sum(&a, &a.files[i]), DIGEST_LEN);
    }
    CHECK(b.files[0].xflags & XMIT_TOP_DIR);
    flist_free(&a);
    flist_free(&b);
    membuf_free(&mb);
}

/* Writes the checksums a receiver holding old[] would send. */
static void put_sums(ars_writer *w, const uint8_t *old, size_t len, int32_t seed)
{
    uint8_t strong[SUM_LENGTH];
    ars_sum_head sh;
    int32_t i, n;

    sum_sizes((int64_t)len, SHORT_SUM_LENGTH, &sh);
    w_int32(w, sh.count);
    w_int32(w, sh.blength);
    w_int32(w, sh.s2length);
    w_int32(w, sh.remainder);
    for (i = 0; i < sh.count; i++) {
        n = i == sh.count - 1 && sh.remainder ? sh.remainder : sh.blength;
        w_int32(w, (int32_t)rolling_sum(old + (size_t)i * (size_t)sh.blength, (size_t)n));
        block_sum(DIGEST_MD5, seed, 1, old + (size_t)i * (size_t)sh.blength, (size_t)n, strong);
        w_bytes(w, strong, (size_t)sh.s2length);
    }
}

/* Rebuilds the file from tokens as a receiver would; checks the trailing
 * file checksum too. Returns the length, or -1 on a malformed stream. */
static long apply_tokens(ars_token_rx *rx, ars_reader *r, const ars_sums *s, const uint8_t *old,
                         size_t oldlen, uint8_t *out, size_t outsize, long *refs)
{
    uint8_t want[SUM_LENGTH], got[SUM_LENGTH];
    const uint8_t *data;
    ars_digest d;
    size_t pos = 0, len;
    int32_t t;
    int err;

    *refs = 0;
    digest_init(&d, DIGEST_MD5);
    while ((t = token_recv(rx, r, &data, &err)) != 0) {
        if (t > 0) {
            if (pos + (size_t)t > outsize)
                return -1;
            memcpy(out + pos, data, (size_t)t);
            pos += (size_t)t;
        } else {
            t = -(t + 1);
            if (t >= s->h.count)
                return -1;
            len = t == s->h.count - 1 && s->h.remainder ? (size_t)s->h.remainder : (size_t)s->h.blength;
            if ((size_t)t * (size_t)s->h.blength + len > oldlen || pos + len > outsize)
                return -1;
            memcpy(out + pos, old + (size_t)t * (size_t)s->h.blength, len);
            if (token_see(rx, out + pos, (int32_t)len) != WIRE_OK)
                return -1;
            pos += len;
            (*refs)++;
        }
    }
    if (err)
        return -1;
    r_bytes(r, want, sizeof want);
    digest_update(&d, out, pos);
    digest_final(&d, got);
    return r->err || memcmp(want, got, sizeof want) ? -1 : (long)pos;
}

static const char *const cpres_names[] = { "plain", "zlib", "zlibx" };

/* Sends new_data as a delta against old[] and rebuilds it, twice in a row
 * through the same token streams (the second file must start afresh). */
static void test_delta_case(const char *name, const uint8_t *old, size_t oldlen,
                            const uint8_t *new_data, size_t newlen, long min_refs, int cpres)
{
    ars_match_ctx m;
    ars_token_tx tx;
    ars_token_rx rx;
    ars_membuf sums_mb, out_mb;
    ars_reader r;
    ars_writer w;
    ars_sums sums;
    plat_file *f;
    uint8_t *rebuilt = malloc(newlen + 1);
    long refs, got;
    int round;

    /* The "local file" the sender reads. */
    CHECK_EQ(plat_open_write("arsync-test.delta", &f), PLAT_OK);
    CHECK_EQ(plat_write(f, new_data, newlen), PLAT_OK);
    plat_close(f);

    membuf_init(&sums_mb);
    membuf_writer(&w, &sums_mb);
    put_sums(&w, old, oldlen, 4242);
    membuf_reader(&r, &sums_mb);
    CHECK_EQ(sums_read(&sums, &r), WIRE_OK);

    memset(&m, 0, sizeof m);
    m.digest_type = DIGEST_MD5;
    m.seed = 4242;
    m.seed_first = 1;
    m.buf = malloc(MATCH_BUFSIZE);
    m.tx = &tx;
    CHECK_EQ(token_tx_init(&tx, cpres), WIRE_OK);
    CHECK_EQ(token_rx_init(&rx, cpres), WIRE_OK);
    membuf_init(&out_mb);
    membuf_writer(&w, &out_mb);
    for (round = 0; round < 2; round++) {
        CHECK_EQ(plat_open_read("arsync-test.delta", &f), PLAT_OK);
        CHECK_EQ(send_delta(&m, f, &sums, &w), WIRE_OK);
        plat_close(f);
    }

    membuf_reader(&r, &out_mb);
    for (round = 0; round < 2; round++) {
        got = apply_tokens(&rx, &r, &sums, old, oldlen, rebuilt, newlen + 1, &refs);
        if (got != (long)newlen || memcmp(rebuilt, new_data, newlen) != 0 || refs < min_refs)
            printf("  delta case \"%s\" (%s, round %d): rebuilt %ld of %lu bytes,"
                   " %ld block refs (want >= %ld)\n", name, cpres_names[cpres], round + 1,
                   got, (unsigned long)newlen, refs, min_refs);
        CHECK_EQ(got, (long)newlen);
        CHECK(got == (long)newlen && memcmp(rebuilt, new_data, newlen) == 0);
        CHECK(refs >= min_refs);
    }
    CHECK_EQ(out_mb.pos, out_mb.len);
    CHECK_EQ(m.literal + m.matched, 2 * (int64_t)newlen);

    token_tx_free(&tx);
    token_rx_free(&rx);
    sums_free(&sums);
    free(m.buf);
    free(rebuilt);
    membuf_free(&sums_mb);
    membuf_free(&out_mb);
    plat_remove("arsync-test.delta");
}

static void test_delta(int cpres)
{
    enum { N = 200000 };
    uint8_t *old = malloc(N + 100), *cur = malloc(N + 100);
    uint32_t x = 12345;
    size_t i;

    for (i = 0; i < N; i++) {
        x = x * 1103515245u + 12345u;   /* bytes of all values, signed and not */
        old[i] = (uint8_t)(x >> 16);
    }
    /* 700-byte blocks; 286 of them in 200,000 bytes. */
    memcpy(cur, old, N);
    test_delta_case("identical", old, N, cur, N, 285, cpres);
    memcpy(cur, old, N);
    memcpy(cur + 100000, "CHANGED!", 8);
    test_delta_case("changed in the middle", old, N, cur, N, 280, cpres);
    memcpy(cur, "inserted", 8);
    memcpy(cur + 8, old, N);
    test_delta_case("bytes inserted at the start", old, N, cur, N + 8, 280, cpres);
    test_delta_case("truncated", old, N, old, N - 1234, 280, cpres);
    for (i = 0; i < N; i++) {
        x = x * 1103515245u + 12345u;
        cur[i] = (uint8_t)(x >> 16);
    }
    test_delta_case("nothing in common", old, N, cur, N, 0, cpres);
    test_delta_case("empty new file", old, N, cur, 0, 0, cpres);
    test_delta_case("empty basis", old, 0, old, N, 0, cpres);
    test_delta_case("small file", old, 10, old, 5, 0, cpres);
    /* Text compresses, and long literal runs span several records. */
    for (i = 0; i < N; i++)
        cur[i] = (uint8_t)"the quick brown fox jumps over the lazy dog\n"[i % 44 + (i / 3001) % 2];
    test_delta_case("text, no basis", old, 0, cur, N, 0, cpres);
    memcpy(old, cur, N);
    memcpy(cur + 5000, "a change", 8);
    test_delta_case("text, changed", old, N, cur, N, 280, cpres);
    free(old);
    free(cur);
}

/* -i strings, as rsync's log.c builds them. */
static void test_itemize(void)
{
    static const struct {
        long iflags;
        uint32_t mode;
        int sending;
        const char *want;
    } v[] = {
        { ITEM_TRANSFER | ITEM_IS_NEW, WIRE_S_IFREG, 0, ">f+++++++++" },
        { ITEM_TRANSFER | ITEM_REPORT_SIZE | ITEM_REPORT_TIME, WIRE_S_IFREG, 0, ">f.st......" },
        { ITEM_TRANSFER | ITEM_REPORT_CHANGE | ITEM_REPORT_TIME, WIRE_S_IFREG, 1, "<fc.t......" },
        { ITEM_LOCAL_CHANGE | ITEM_IS_NEW, WIRE_S_IFDIR, 0, "cd+++++++++" },
        { ITEM_REPORT_TIME, WIRE_S_IFDIR, 1, ".d..t......" },
        { 0, WIRE_S_IFREG, 0, ".f         " },
        { 0, WIRE_S_IFLNK, 0, ".L         " },
        { ITEM_TRANSFER, WIRE_S_IFREG, 0, ">f........." },
    };
    char buf[12];
    size_t i;

    for (i = 0; i < sizeof v / sizeof v[0]; i++)
        CHECK(strcmp(itemize_string(v[i].iflags, v[i].mode, v[i].sending, buf), v[i].want) == 0);
    /* Shown without -i: changed directories and local changes only. */
    CHECK(item_shown(0, 1, ITEM_REPORT_TIME, WIRE_S_IFDIR));
    CHECK(!item_shown(0, 1, ITEM_REPORT_TIME, WIRE_S_IFREG));
    CHECK(item_shown(1, 0, ITEM_REPORT_TIME, WIRE_S_IFREG));
    CHECK(!item_shown(1, 0, 0, WIRE_S_IFREG));
    CHECK(item_shown(2, 0, 0, WIRE_S_IFREG));
    CHECK(item_shown(1, 2, 0, WIRE_S_IFREG));
}

void test_push_suite(void)
{
    test_flist_roundtrip(1, 0);
    test_flist_roundtrip(0, 0);
    test_flist_roundtrip(1, 1);
    test_flist_roundtrip(0, 1);
    test_itemize();
    test_delta(CPRES_NONE);
    test_delta(CPRES_ZLIB);
    test_delta(CPRES_ZLIBX);
}
