/* test_zlib.c - the zlib build itself, for miscompiles the self-test misses
 *
 * GCC 6.5's m68k peephole pass merges inffast.c's byte-at-a-time match copy
 * into a word move. When a match starts in the history window and carries
 * on into the output at distance 1, source and destination overlap, so the
 * word move reads a byte before it has been written and copies whatever the
 * output buffer held before. Seen as a file failing verification twice
 * with -z; inffast.c is built at -O1 for it (see the Makefile).
 */
#include <stdint.h>

#include "test.h"
#include "zlib.h"

#define PIECES 40
#define PIECE_LEN 500

/* Compresses PIECES pieces with a sync flush after each, as rsync's sender
 * does per literal token, then inflates them one call per piece into a
 * buffer that holds junk first, as a reused receive buffer does. Each piece
 * after the first starts with a run of the byte the previous piece ended
 * with, so it opens with a distance-1 match reaching into the window. */
static void test_overlapping_match(void)
{
    static uint8_t raw[PIECES * PIECE_LEN], packed[PIECES * PIECE_LEN + 1024];
    static uint8_t out[PIECE_LEN + 64];
    size_t ends[PIECES], i, start = 0;
    z_stream z;
    int k, zr;

    for (k = 0; k < PIECES; k++)
        for (i = 0; i < PIECE_LEN; i++)
            raw[k * PIECE_LEN + i] = i < PIECE_LEN - 3 ? (uint8_t)(0x41 + k) : (uint8_t)(0x42 + k);

    memset(&z, 0, sizeof z);
    CHECK_EQ(deflateInit2(&z, Z_DEFAULT_COMPRESSION, Z_DEFLATED, -15, 8, Z_DEFAULT_STRATEGY), Z_OK);
    z.next_out = packed;
    z.avail_out = sizeof packed;
    for (k = 0; k < PIECES; k++) {
        z.next_in = raw + k * PIECE_LEN;
        z.avail_in = PIECE_LEN;
        CHECK_EQ(deflate(&z, Z_SYNC_FLUSH), Z_OK);
        ends[k] = z.total_out;
    }
    deflateEnd(&z);

    memset(&z, 0, sizeof z);
    CHECK_EQ(inflateInit2(&z, -15), Z_OK);
    for (k = 0; k < PIECES; k++) {
        memset(out, 0xEE, sizeof out);
        z.next_in = packed + start;
        z.avail_in = (uInt)(ends[k] - start);
        z.next_out = out;
        z.avail_out = sizeof out;
        zr = inflate(&z, Z_SYNC_FLUSH);
        CHECK(zr == Z_OK || zr == Z_BUF_ERROR);
        CHECK_EQ(sizeof out - z.avail_out, PIECE_LEN);
        CHECK_MEM(out, raw + k * PIECE_LEN, PIECE_LEN);
        start = ends[k];
    }
    inflateEnd(&z);
}

void test_zlib_suite(void)
{
    test_overlapping_match();
}
