/* match.c - the sending side of a delta
 *
 * The receiver describes its copy of the file as blocks (weak and strong
 * checksums). We slide a window of one block's length over our file,
 * keeping the weak "rolling" checksum up to date one byte at a time; when
 * it matches a block's, the strong checksum decides. A match is sent as a
 * block reference, everything between matches as literal data (see
 * token.c for how they're encoded), then the whole-file checksum.
 */
#include <stdlib.h>
#include <string.h>

#include "match.h"
#include "digest.h"

#define MAX_BLOCKS (1L << 24)   /* sanity limit: 16 M blocks */

void sums_free(ars_sums *s)
{
    free(s->sum1);
    free(s->sum2);
    free(s->head);
    free(s->next);
    memset(s, 0, sizeof *s);
}

static uint32_t hash_of(const ars_sums *s, uint32_t sum1)
{
    return (sum1 ^ (sum1 >> 16) ^ (sum1 >> 8)) & s->mask;
}

int sums_read(ars_sums *s, ars_reader *r)
{
    int32_t i;
    uint32_t size;

    memset(s, 0, sizeof *s);
    s->h.count = r_int32(r);
    s->h.blength = r_int32(r);
    s->h.s2length = r_int32(r);
    s->h.remainder = r_int32(r);
    if (r->err)
        return r->err;
    if (s->h.count < 0 || s->h.count > MAX_BLOCKS || s->h.blength < 0
        || s->h.blength > 131072 || s->h.s2length < 0 || s->h.s2length > SUM_LENGTH
        || s->h.remainder < 0 || s->h.remainder > s->h.blength
        || (s->h.count > 0 && s->h.blength == 0))
        return WIRE_EPROTO;
    if (s->h.count == 0)
        return WIRE_OK;

    for (size = 16; size < (uint32_t)s->h.count; size <<= 1)
        ;
    s->mask = size - 1;
    s->sum1 = malloc((size_t)s->h.count * sizeof *s->sum1);
    s->sum2 = malloc((size_t)s->h.count * (size_t)s->h.s2length + 1);
    s->head = malloc(size * sizeof *s->head);
    s->next = malloc((size_t)s->h.count * sizeof *s->next);
    if (!s->sum1 || !s->sum2 || !s->head || !s->next) {
        sums_free(s);
        return WIRE_ENOMEM;
    }
    memset(s->head, 0xFF, size * sizeof *s->head);   /* all -1 */
    for (i = 0; i < s->h.count; i++) {
        s->sum1[i] = (uint32_t)r_int32(r);
        r_bytes(r, s->sum2 + (size_t)i * (size_t)s->h.s2length, (size_t)s->h.s2length);
    }
    if (r->err) {
        sums_free(s);
        return r->err;
    }
    /* Chain in reverse so each bucket lists lower block numbers first. */
    for (i = s->h.count - 1; i >= 0; i--) {
        uint32_t h = hash_of(s, s->sum1[i]);

        s->next[i] = s->head[h];
        s->head[h] = i;
    }
    return WIRE_OK;
}

/* The file is read through a window: m->buf holds bytes [win_off,
 * win_off + win_len) of the file. */
typedef struct window {
    plat_file *f;
    int64_t off;
    size_t len;
    int eof;
} window;

/* Makes sure the window holds [from, to) (or up to end of file), keeping
 * everything from 'from' on. */
static void fill(ars_match_ctx *m, window *wd, int64_t from, int64_t to)
{
    size_t keep;
    long r;

    if (to <= wd->off + (int64_t)wd->len || wd->eof)
        return;
    keep = (size_t)(wd->off + (int64_t)wd->len - from);
    memmove(m->buf, m->buf + (from - wd->off), keep);
    wd->off = from;
    wd->len = keep;
    while (wd->len < MATCH_BUFSIZE) {
        r = plat_read(wd->f, m->buf + wd->len, MATCH_BUFSIZE - wd->len);
        if (r < 0)
            m->read_error = 1;
        if (r <= 0) {
            wd->eof = 1;
            break;
        }
        wd->len += (size_t)r;
    }
}

/* Sends the literal data [lit, lit + n) and then a token (see token.h);
 * block is the matched block's data, for compression. */
static int send_piece(ars_match_ctx *m, ars_writer *w, ars_digest *sum, int32_t token,
                      const uint8_t *lit, size_t n, const uint8_t *block, size_t blen)
{
    if (n > 0) {
        digest_update(sum, lit, n);
        m->literal += (int64_t)n;
    }
    return token_send(m->tx, w, token, lit, n, block, blen);
}

/* Index of a block of length n whose checksums match data, or -1. */
static int32_t find_block(ars_match_ctx *m, const ars_sums *s, uint32_t weak,
                          const uint8_t *data, int32_t n)
{
    uint8_t strong[SUM_LENGTH];
    int have_strong = 0;
    int32_t i, len;

    for (i = s->head[hash_of(s, weak)]; i >= 0; i = s->next[i]) {
        if (s->sum1[i] != weak)
            continue;
        len = i == s->h.count - 1 && s->h.remainder ? s->h.remainder : s->h.blength;
        if (len != n)
            continue;
        if (!have_strong) {
            block_sum(m->digest_type, m->seed, m->seed_first, data, (size_t)n, strong);
            have_strong = 1;
        }
        if (memcmp(strong, s->sum2 + (size_t)i * (size_t)s->h.s2length, (size_t)s->h.s2length) == 0)
            return i;
    }
    return -1;
}

int send_delta(ars_match_ctx *m, plat_file *f, const ars_sums *s, ars_writer *w)
{
    uint8_t file_sum[SUM_LENGTH];
    ars_digest sum;
    window wd;
    int64_t pos = 0, lit = 0;     /* window start; start of pending literal */
    int64_t end;
    uint32_t s1 = 0, s2 = 0, weak;
    int32_t n = 0, blk, i;
    int fresh = 1, rc;
    uint8_t *p;

    memset(&wd, 0, sizeof wd);
    wd.f = f;
    digest_init(&sum, m->digest_type);

    for (;;) {
        if (w->err)
            return w->err;

        /* Keep the pending literal small, and the window's next block in
         * the buffer. */
        if (pos - lit >= CHUNK_BUFSIZE) {
            fill(m, &wd, lit, pos);
            if ((rc = send_piece(m, w, &sum, -2, m->buf + (lit - wd.off), (size_t)(pos - lit),
                                 NULL, 0)) != WIRE_OK)
                return rc;
            lit = pos;
            if (m->progress)
                m->progress(m->progress_ctx, pos);
            if (plat_break_pending())
                return WIRE_EABORT;
        }
        fill(m, &wd, lit, pos + (s->h.count ? s->h.blength + 1 : CHUNK_BUFSIZE));
        end = wd.off + (int64_t)wd.len;
        if (pos >= end)
            break;

        if (s->h.count == 0) {
            /* No blocks to look for: everything is literal. */
            pos = end;
            continue;
        }

        p = m->buf + (pos - wd.off);
        if (fresh) {
            n = end - pos < s->h.blength ? (int32_t)(end - pos) : s->h.blength;
            s1 = s2 = 0;
            for (i = 0; i < n; i++) {
                s1 += (uint32_t)(int32_t)(int8_t)p[i];
                s2 += s1;
            }
            fresh = 0;
        }

        /* Most positions hit an empty bucket; skip the call for those. */
        weak = (s1 & 0xFFFF) + (s2 << 16);
        blk = s->head[hash_of(s, weak)] < 0 ? -1 : find_block(m, s, weak, p, n);
        if (blk >= 0) {
            if ((rc = send_piece(m, w, &sum, blk, m->buf + (lit - wd.off), (size_t)(pos - lit),
                                 p, (size_t)n)) != WIRE_OK)
                return rc;
            digest_update(&sum, p, (size_t)n);
            m->matched += n;
            pos += n;
            lit = pos;
            fresh = 1;
            continue;
        }

        /* Slide one byte: drop p[0], take in p[n] if there is one. */
        s1 -= (uint32_t)(int32_t)(int8_t)p[0];
        s2 -= (uint32_t)n * (uint32_t)(int32_t)(int8_t)p[0];
        if (pos + n < end) {
            s1 += (uint32_t)(int32_t)(int8_t)p[n];
            s2 += s1;
        } else {
            n--;
        }
        pos++;
        if (n == 0)
            fresh = 1;
    }

    fill(m, &wd, lit, pos);
    if ((rc = send_piece(m, w, &sum, -1, m->buf + (lit - wd.off), (size_t)(pos - lit),
                         NULL, 0)) != WIRE_OK)
        return rc;
    digest_final(&sum, file_sum);
    w_bytes(w, file_sum, SUM_LENGTH);
    if (m->progress)
        m->progress(m->progress_ctx, pos);
    return w->err;
}
