/* checksum.c - delta-transfer checksums */
#include "checksum.h"
#include "digest.h"

#define MAX_BLOCK_SIZE ((int32_t)1 << 17)
#define BLOCKSUM_BIAS 10

static int ilog2_32(uint32_t v)
{
    int n = 0;

    while (v >>= 1)
        n++;
    return n;
}

/* floor(log2(v)) for v > 0. Done on 32-bit halves on purpose: GCC 6.5 for
 * m68k miscompiles "while (v >>= 1)" on a 64-bit v at -O2 (the loop tests
 * the flags of the low word's roxr only), giving 0 for e.g. 2^40. */
static int ilog2(uint64_t v)
{
    uint32_t hi = (uint32_t)(v >> 32);

    return hi ? 32 + ilog2_32(hi) : ilog2_32((uint32_t)v);
}

int sum_sizes(int64_t len, int min_s2length, ars_sum_head *sh)
{
    int32_t blength, bit;
    int64_t count;
    int b;

    if (len < 0)
        return -1;

    if (len <= (int64_t)BLOCK_SIZE * BLOCK_SIZE) {
        blength = BLOCK_SIZE;
    } else {
        /* Largest multiple of 8 whose square doesn't exceed len, found bit
         * by bit from 2^floor(log4 len) down; capped at 128 KiB. */
        bit = ilog2((uint64_t)len) / 2 >= 17 ? MAX_BLOCK_SIZE : (int32_t)1 << (ilog2((uint64_t)len) / 2);
        if (bit >= MAX_BLOCK_SIZE) {
            blength = MAX_BLOCK_SIZE;
        } else {
            blength = 0;
            for (; bit >= 8; bit >>= 1) {
                blength |= bit;
                if (len < (int64_t)blength * blength)
                    blength &= ~bit;
            }
            if (blength < BLOCK_SIZE)
                blength = BLOCK_SIZE;
        }
    }

    if (min_s2length >= SUM_LENGTH) {
        sh->s2length = SUM_LENGTH;
    } else {
        /* Bits needed: 10 + 2*log2(len) - log2(blength), less the 32 the
         * weak checksum already gives, plus one, in whole bytes. (C's
         * truncating division is part of the definition.) */
        b = BLOCKSUM_BIAS + 2 * (len > 0 ? ilog2((uint64_t)len) : 0) - ilog2((uint64_t)blength);
        if (b < 0)
            b = 0;
        sh->s2length = (b + 1 - 32 + 7) / 8;
        if (sh->s2length < min_s2length)
            sh->s2length = min_s2length;
        if (sh->s2length > SUM_LENGTH)
            sh->s2length = SUM_LENGTH;
    }

    count = len / blength + (len % blength != 0);
    if (count > 0x7FFFFFFF)
        return -1;
    sh->blength = blength;
    sh->remainder = (int32_t)(len % blength);
    sh->count = (int32_t)count;
    return 0;
}

uint32_t rolling_sum(const uint8_t *buf, size_t len)
{
    const int8_t *b = (const int8_t *)buf;
    uint32_t s1 = 0, s2 = 0;
    size_t i = 0;

    /* Four bytes per round: s2 gains 4*s1 + 4*b0 + 3*b1 + 2*b2 + b3. */
    for (; i + 4 <= len; i += 4) {
        s2 += 4 * (s1 + (uint32_t)b[i]) + 3 * (uint32_t)b[i + 1] + 2 * (uint32_t)b[i + 2]
              + (uint32_t)b[i + 3];
        s1 += (uint32_t)b[i] + (uint32_t)b[i + 1] + (uint32_t)b[i + 2] + (uint32_t)b[i + 3];
    }
    for (; i < len; i++) {
        s1 += (uint32_t)b[i];
        s2 += s1;
    }
    return (s1 & 0xFFFF) + (s2 << 16);
}

void block_sum(int digest_type, int32_t seed, int seed_first,
               const uint8_t *buf, size_t len, uint8_t out[SUM_LENGTH])
{
    uint8_t s[4];
    ars_digest d;

    s[0] = (uint8_t)seed;
    s[1] = (uint8_t)((uint32_t)seed >> 8);
    s[2] = (uint8_t)((uint32_t)seed >> 16);
    s[3] = (uint8_t)((uint32_t)seed >> 24);

    digest_init(&d, digest_type);
    if (seed && seed_first)
        digest_update(&d, s, 4);
    digest_update(&d, buf, len);
    if (seed && !seed_first)
        digest_update(&d, s, 4);
    digest_final(&d, out);
}
