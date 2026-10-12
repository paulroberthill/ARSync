/* test_push.c - the sending side: file-list encoding and delta matching */
#include <stdlib.h>

#include "test.h"
#include "checksum.h"
#include "digest.h"
#include "flist.h"
#include "match.h"
#include "plat.h"

/* What a sender encodes, a receiver must decode to the same list. */
static void test_flist_roundtrip(int varint)
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
    flist_add(&a, ".", WIRE_S_IFDIR | 0755, 0, 1700000000, 0);
    a.files[0].xflags = XMIT_TOP_DIR;
    flist_add(&a, "dir", WIRE_S_IFDIR | 0755, 0, 1700000000, 0);
    flist_add(&a, "dir/file-a", WIRE_S_IFREG | 0644, 5000000000LL, 1700000001, 999);
    flist_add(&a, "dir/file-b", WIRE_S_IFREG | 0644, 0, 1700000001, 0);
    flist_add(&a, longname, WIRE_S_IFREG | 0600, 7, -5, 0);
    flist_add(&a, "link", WIRE_S_IFLNK | 0777, 1, 1, 0);
    flist_sort_and_clean(&a);

    fmt.varint_flags = varint;
    fmt.safe_flist = 1;
    membuf_init(&mb);
    membuf_writer(&w, &mb);
    flist_send(&a, &w, &fmt, 32, IOERR_GENERAL);
    flist_init(&b);
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
static long apply_tokens(ars_reader *r, const ars_sums *s, const uint8_t *old, size_t oldlen,
                         uint8_t *out, size_t outsize, long *refs)
{
    uint8_t want[SUM_LENGTH], got[SUM_LENGTH];
    ars_digest d;
    size_t pos = 0, len;
    int32_t t;

    *refs = 0;
    digest_init(&d, DIGEST_MD5);
    while ((t = r_int32(r)) != 0 && !r->err) {
        if (t > 0) {
            if (pos + (size_t)t > outsize)
                return -1;
            r_bytes(r, out + pos, (size_t)t);
            pos += (size_t)t;
        } else {
            t = -(t + 1);
            if (t >= s->h.count)
                return -1;
            len = t == s->h.count - 1 && s->h.remainder ? (size_t)s->h.remainder : (size_t)s->h.blength;
            if ((size_t)t * (size_t)s->h.blength + len > oldlen || pos + len > outsize)
                return -1;
            memcpy(out + pos, old + (size_t)t * (size_t)s->h.blength, len);
            pos += len;
            (*refs)++;
        }
    }
    r_bytes(r, want, sizeof want);
    digest_update(&d, out, pos);
    digest_final(&d, got);
    return r->err || memcmp(want, got, sizeof want) ? -1 : (long)pos;
}

static void test_delta_case(const char *name, const uint8_t *old, size_t oldlen,
                            const uint8_t *new_data, size_t newlen, long min_refs)
{
    ars_match_ctx m;
    ars_membuf sums_mb, out_mb;
    ars_reader r;
    ars_writer w;
    ars_sums sums;
    plat_file *f;
    uint8_t *rebuilt = malloc(newlen + 1);
    long refs, got;

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
    membuf_init(&out_mb);
    membuf_writer(&w, &out_mb);
    CHECK_EQ(plat_open_read("arsync-test.delta", &f), PLAT_OK);
    CHECK_EQ(send_delta(&m, f, &sums, &w), WIRE_OK);
    plat_close(f);

    membuf_reader(&r, &out_mb);
    got = apply_tokens(&r, &sums, old, oldlen, rebuilt, newlen + 1, &refs);
    if (got != (long)newlen || memcmp(rebuilt, new_data, newlen) != 0 || refs < min_refs)
        printf("  delta case \"%s\": rebuilt %ld of %lu bytes, %ld block refs (want >= %ld)\n",
               name, got, (unsigned long)newlen, refs, min_refs);
    CHECK_EQ(got, (long)newlen);
    CHECK(got == (long)newlen && memcmp(rebuilt, new_data, newlen) == 0);
    CHECK(refs >= min_refs);
    CHECK_EQ(m.literal + m.matched, (int64_t)newlen);

    sums_free(&sums);
    free(m.buf);
    free(rebuilt);
    membuf_free(&sums_mb);
    membuf_free(&out_mb);
    plat_remove("arsync-test.delta");
}

static void test_delta(void)
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
    test_delta_case("identical", old, N, cur, N, 285);
    memcpy(cur, old, N);
    memcpy(cur + 100000, "CHANGED!", 8);
    test_delta_case("changed in the middle", old, N, cur, N, 280);
    memcpy(cur, "inserted", 8);
    memcpy(cur + 8, old, N);
    test_delta_case("bytes inserted at the start", old, N, cur, N + 8, 280);
    test_delta_case("truncated", old, N, old, N - 1234, 280);
    for (i = 0; i < N; i++) {
        x = x * 1103515245u + 12345u;
        cur[i] = (uint8_t)(x >> 16);
    }
    test_delta_case("nothing in common", old, N, cur, N, 0);
    test_delta_case("empty new file", old, N, cur, 0, 0);
    test_delta_case("empty basis", old, 0, old, N, 0);
    test_delta_case("small file", old, 10, old, 5, 0);
    free(old);
    free(cur);
}

void test_push_suite(void)
{
    test_flist_roundtrip(1);
    test_flist_roundtrip(0);
    test_delta();
}
