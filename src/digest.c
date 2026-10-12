/* digest.c - MD4 (RFC 1320) and MD5 (RFC 1321) message digests
 *
 * Both work on 64-byte blocks of little-endian 32-bit words and share the
 * same padding and buffering; only the block function differs. Words are
 * assembled byte by byte, so this is correct on the big-endian 68k.
 */
#include <string.h>

#include "digest.h"

/* A function, not a macro, so the value is cut to 32 bits before rotating
 * even where unsigned long (the constants' type) is 64 bits wide. */
static inline uint32_t ROL(uint32_t x, int n)
{
    return (x << n) | (x >> (32 - n));
}

static void load_words(uint32_t w[16], const uint8_t *p)
{
    int i;

    for (i = 0; i < 16; i++, p += 4)
        w[i] = (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

/* The step functions are written out in full (no loops or tables in the
 * rounds): on a 68k this is several times faster than a compact loop, and
 * these run over every byte of every file in a delta transfer. */

#define F4(x, y, z) (((x) & (y)) | (~(x) & (z)))
#define G4(x, y, z) (((x) & (y)) | ((x) & (z)) | ((y) & (z)))
#define H4(x, y, z) ((x) ^ (y) ^ (z))

#define R1(a, b, c, d, k, s) a = ROL(a + F4(b, c, d) + w[k], s)
#define R2(a, b, c, d, k, s) a = ROL(a + G4(b, c, d) + w[k] + 0x5A827999UL, s)
#define R3(a, b, c, d, k, s) a = ROL(a + H4(b, c, d) + w[k] + 0x6ED9EBA1UL, s)

static void md4_block(uint32_t s[4], const uint8_t *p)
{
    uint32_t w[16], a = s[0], b = s[1], c = s[2], d = s[3];

    load_words(w, p);

    R1(a, b, c, d, 0, 3);  R1(d, a, b, c, 1, 7);  R1(c, d, a, b, 2, 11);  R1(b, c, d, a, 3, 19);
    R1(a, b, c, d, 4, 3);  R1(d, a, b, c, 5, 7);  R1(c, d, a, b, 6, 11);  R1(b, c, d, a, 7, 19);
    R1(a, b, c, d, 8, 3);  R1(d, a, b, c, 9, 7);  R1(c, d, a, b, 10, 11); R1(b, c, d, a, 11, 19);
    R1(a, b, c, d, 12, 3); R1(d, a, b, c, 13, 7); R1(c, d, a, b, 14, 11); R1(b, c, d, a, 15, 19);

    R2(a, b, c, d, 0, 3);  R2(d, a, b, c, 4, 5);  R2(c, d, a, b, 8, 9);   R2(b, c, d, a, 12, 13);
    R2(a, b, c, d, 1, 3);  R2(d, a, b, c, 5, 5);  R2(c, d, a, b, 9, 9);   R2(b, c, d, a, 13, 13);
    R2(a, b, c, d, 2, 3);  R2(d, a, b, c, 6, 5);  R2(c, d, a, b, 10, 9);  R2(b, c, d, a, 14, 13);
    R2(a, b, c, d, 3, 3);  R2(d, a, b, c, 7, 5);  R2(c, d, a, b, 11, 9);  R2(b, c, d, a, 15, 13);

    R3(a, b, c, d, 0, 3);  R3(d, a, b, c, 8, 9);  R3(c, d, a, b, 4, 11);  R3(b, c, d, a, 12, 15);
    R3(a, b, c, d, 2, 3);  R3(d, a, b, c, 10, 9); R3(c, d, a, b, 6, 11);  R3(b, c, d, a, 14, 15);
    R3(a, b, c, d, 1, 3);  R3(d, a, b, c, 9, 9);  R3(c, d, a, b, 5, 11);  R3(b, c, d, a, 13, 15);
    R3(a, b, c, d, 3, 3);  R3(d, a, b, c, 11, 9); R3(c, d, a, b, 7, 11);  R3(b, c, d, a, 15, 15);

    s[0] += a;
    s[1] += b;
    s[2] += c;
    s[3] += d;
}

#define F5(x, y, z) ((z) ^ ((x) & ((y) ^ (z))))
#define G5(x, y, z) ((y) ^ ((z) & ((x) ^ (y))))
#define H5(x, y, z) ((x) ^ (y) ^ (z))
#define I5(x, y, z) ((y) ^ ((x) | ~(z)))

/* One MD5 step; k is the sine-derived constant (RFC 1321's T[i]). */
#define STEP(f, a, b, c, d, x, k, s) a = b + ROL(a + f(b, c, d) + (x) + (k), s)

static void md5_block(uint32_t s[4], const uint8_t *p)
{
    uint32_t w[16], a = s[0], b = s[1], c = s[2], d = s[3];

    load_words(w, p);

    STEP(F5, a, b, c, d, w[0], 0xd76aa478UL, 7);
    STEP(F5, d, a, b, c, w[1], 0xe8c7b756UL, 12);
    STEP(F5, c, d, a, b, w[2], 0x242070dbUL, 17);
    STEP(F5, b, c, d, a, w[3], 0xc1bdceeeUL, 22);
    STEP(F5, a, b, c, d, w[4], 0xf57c0fafUL, 7);
    STEP(F5, d, a, b, c, w[5], 0x4787c62aUL, 12);
    STEP(F5, c, d, a, b, w[6], 0xa8304613UL, 17);
    STEP(F5, b, c, d, a, w[7], 0xfd469501UL, 22);
    STEP(F5, a, b, c, d, w[8], 0x698098d8UL, 7);
    STEP(F5, d, a, b, c, w[9], 0x8b44f7afUL, 12);
    STEP(F5, c, d, a, b, w[10], 0xffff5bb1UL, 17);
    STEP(F5, b, c, d, a, w[11], 0x895cd7beUL, 22);
    STEP(F5, a, b, c, d, w[12], 0x6b901122UL, 7);
    STEP(F5, d, a, b, c, w[13], 0xfd987193UL, 12);
    STEP(F5, c, d, a, b, w[14], 0xa679438eUL, 17);
    STEP(F5, b, c, d, a, w[15], 0x49b40821UL, 22);

    STEP(G5, a, b, c, d, w[1], 0xf61e2562UL, 5);
    STEP(G5, d, a, b, c, w[6], 0xc040b340UL, 9);
    STEP(G5, c, d, a, b, w[11], 0x265e5a51UL, 14);
    STEP(G5, b, c, d, a, w[0], 0xe9b6c7aaUL, 20);
    STEP(G5, a, b, c, d, w[5], 0xd62f105dUL, 5);
    STEP(G5, d, a, b, c, w[10], 0x02441453UL, 9);
    STEP(G5, c, d, a, b, w[15], 0xd8a1e681UL, 14);
    STEP(G5, b, c, d, a, w[4], 0xe7d3fbc8UL, 20);
    STEP(G5, a, b, c, d, w[9], 0x21e1cde6UL, 5);
    STEP(G5, d, a, b, c, w[14], 0xc33707d6UL, 9);
    STEP(G5, c, d, a, b, w[3], 0xf4d50d87UL, 14);
    STEP(G5, b, c, d, a, w[8], 0x455a14edUL, 20);
    STEP(G5, a, b, c, d, w[13], 0xa9e3e905UL, 5);
    STEP(G5, d, a, b, c, w[2], 0xfcefa3f8UL, 9);
    STEP(G5, c, d, a, b, w[7], 0x676f02d9UL, 14);
    STEP(G5, b, c, d, a, w[12], 0x8d2a4c8aUL, 20);

    STEP(H5, a, b, c, d, w[5], 0xfffa3942UL, 4);
    STEP(H5, d, a, b, c, w[8], 0x8771f681UL, 11);
    STEP(H5, c, d, a, b, w[11], 0x6d9d6122UL, 16);
    STEP(H5, b, c, d, a, w[14], 0xfde5380cUL, 23);
    STEP(H5, a, b, c, d, w[1], 0xa4beea44UL, 4);
    STEP(H5, d, a, b, c, w[4], 0x4bdecfa9UL, 11);
    STEP(H5, c, d, a, b, w[7], 0xf6bb4b60UL, 16);
    STEP(H5, b, c, d, a, w[10], 0xbebfbc70UL, 23);
    STEP(H5, a, b, c, d, w[13], 0x289b7ec6UL, 4);
    STEP(H5, d, a, b, c, w[0], 0xeaa127faUL, 11);
    STEP(H5, c, d, a, b, w[3], 0xd4ef3085UL, 16);
    STEP(H5, b, c, d, a, w[6], 0x04881d05UL, 23);
    STEP(H5, a, b, c, d, w[9], 0xd9d4d039UL, 4);
    STEP(H5, d, a, b, c, w[12], 0xe6db99e5UL, 11);
    STEP(H5, c, d, a, b, w[15], 0x1fa27cf8UL, 16);
    STEP(H5, b, c, d, a, w[2], 0xc4ac5665UL, 23);

    STEP(I5, a, b, c, d, w[0], 0xf4292244UL, 6);
    STEP(I5, d, a, b, c, w[7], 0x432aff97UL, 10);
    STEP(I5, c, d, a, b, w[14], 0xab9423a7UL, 15);
    STEP(I5, b, c, d, a, w[5], 0xfc93a039UL, 21);
    STEP(I5, a, b, c, d, w[12], 0x655b59c3UL, 6);
    STEP(I5, d, a, b, c, w[3], 0x8f0ccc92UL, 10);
    STEP(I5, c, d, a, b, w[10], 0xffeff47dUL, 15);
    STEP(I5, b, c, d, a, w[1], 0x85845dd1UL, 21);
    STEP(I5, a, b, c, d, w[8], 0x6fa87e4fUL, 6);
    STEP(I5, d, a, b, c, w[15], 0xfe2ce6e0UL, 10);
    STEP(I5, c, d, a, b, w[6], 0xa3014314UL, 15);
    STEP(I5, b, c, d, a, w[13], 0x4e0811a1UL, 21);
    STEP(I5, a, b, c, d, w[4], 0xf7537e82UL, 6);
    STEP(I5, d, a, b, c, w[11], 0xbd3af235UL, 10);
    STEP(I5, c, d, a, b, w[2], 0x2ad7d2bbUL, 15);
    STEP(I5, b, c, d, a, w[9], 0xeb86d391UL, 21);

    s[0] += a;
    s[1] += b;
    s[2] += c;
    s[3] += d;
}

static void run_block(ars_digest *d, const uint8_t *p)
{
    if (d->type == DIGEST_MD4)
        md4_block(d->state, p);
    else
        md5_block(d->state, p);
}

void digest_init(ars_digest *d, int type)
{
    d->type = type;
    d->state[0] = 0x67452301UL;
    d->state[1] = 0xefcdab89UL;
    d->state[2] = 0x98badcfeUL;
    d->state[3] = 0x10325476UL;
    d->total = 0;
}

void digest_update(ars_digest *d, const void *data, size_t len)
{
    const uint8_t *p = data;
    size_t used = (size_t)(d->total & 63), take;

    d->total += len;
    if (used) {
        take = 64 - used < len ? 64 - used : len;
        memcpy(d->block + used, p, take);
        p += take;
        len -= take;
        if (used + take < 64)
            return;
        run_block(d, d->block);
    }
    for (; len >= 64; p += 64, len -= 64)
        run_block(d, p);
    memcpy(d->block, p, len);
}

void digest_final(ars_digest *d, uint8_t out[DIGEST_LEN])
{
    static const uint8_t pad[64] = { 0x80 };
    uint8_t lenbytes[8];
    uint64_t bits = d->total * 8;
    size_t used = (size_t)(d->total & 63);
    int i;

    for (i = 0; i < 8; i++)
        lenbytes[i] = (uint8_t)(bits >> (8 * i));
    /* Pad to 56 mod 64, then append the bit length little-endian. */
    digest_update(d, pad, used < 56 ? 56 - used : 120 - used);
    digest_update(d, lenbytes, 8);

    for (i = 0; i < 16; i++)
        out[i] = (uint8_t)(d->state[i >> 2] >> (8 * (i & 3)));
}

int digest_by_name(const char *name, size_t len)
{
    if (len == 3 && memcmp(name, "md5", 3) == 0)
        return DIGEST_MD5;
    if (len == 3 && memcmp(name, "md4", 3) == 0)
        return DIGEST_MD4;
    return 0;
}
