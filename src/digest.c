/* digest.c - MD4 (RFC 1320) and MD5 (RFC 1321) message digests
 *
 * Both work on 64-byte blocks of little-endian 32-bit words and share the
 * same padding and buffering; only the block function differs. Words are
 * assembled byte by byte, so this is correct on the big-endian 68k.
 */
#include <string.h>

#include "digest.h"

#define ROL(x, n) (((x) << (n)) | ((x) >> (32 - (n))))

static void load_words(uint32_t w[16], const uint8_t *p)
{
    int i;

    for (i = 0; i < 16; i++, p += 4)
        w[i] = (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static void md4_block(uint32_t s[4], const uint8_t *p)
{
    static const uint8_t r2[16] = { 0, 4, 8, 12, 1, 5, 9, 13, 2, 6, 10, 14, 3, 7, 11, 15 };
    static const uint8_t r3[16] = { 0, 8, 4, 12, 2, 10, 6, 14, 1, 9, 5, 13, 3, 11, 7, 15 };
    static const uint8_t sh1[4] = { 3, 7, 11, 19 };
    static const uint8_t sh2[4] = { 3, 5, 9, 13 };
    static const uint8_t sh3[4] = { 3, 9, 11, 15 };
    uint32_t w[16], a = s[0], b = s[1], c = s[2], d = s[3], t;
    int i;

    load_words(w, p);
    for (i = 0; i < 48; i++) {
        if (i < 16)
            t = a + ((b & c) | (~b & d)) + w[i];
        else if (i < 32)
            t = a + ((b & c) | (b & d) | (c & d)) + w[r2[i - 16]] + 0x5A827999UL;
        else
            t = a + (b ^ c ^ d) + w[r3[i - 32]] + 0x6ED9EBA1UL;
        t = ROL(t, (i < 16 ? sh1 : i < 32 ? sh2 : sh3)[i & 3]);
        /* Rotate the roles: the new value becomes b's predecessor. */
        a = d;
        d = c;
        c = b;
        b = t;
    }
    s[0] += a;
    s[1] += b;
    s[2] += c;
    s[3] += d;
}

static void md5_block(uint32_t s[4], const uint8_t *p)
{
    /* K[i] = floor(|sin(i + 1)| * 2^32) */
    static const uint32_t k[64] = {
        0xd76aa478UL, 0xe8c7b756UL, 0x242070dbUL, 0xc1bdceeeUL,
        0xf57c0fafUL, 0x4787c62aUL, 0xa8304613UL, 0xfd469501UL,
        0x698098d8UL, 0x8b44f7afUL, 0xffff5bb1UL, 0x895cd7beUL,
        0x6b901122UL, 0xfd987193UL, 0xa679438eUL, 0x49b40821UL,
        0xf61e2562UL, 0xc040b340UL, 0x265e5a51UL, 0xe9b6c7aaUL,
        0xd62f105dUL, 0x02441453UL, 0xd8a1e681UL, 0xe7d3fbc8UL,
        0x21e1cde6UL, 0xc33707d6UL, 0xf4d50d87UL, 0x455a14edUL,
        0xa9e3e905UL, 0xfcefa3f8UL, 0x676f02d9UL, 0x8d2a4c8aUL,
        0xfffa3942UL, 0x8771f681UL, 0x6d9d6122UL, 0xfde5380cUL,
        0xa4beea44UL, 0x4bdecfa9UL, 0xf6bb4b60UL, 0xbebfbc70UL,
        0x289b7ec6UL, 0xeaa127faUL, 0xd4ef3085UL, 0x04881d05UL,
        0xd9d4d039UL, 0xe6db99e5UL, 0x1fa27cf8UL, 0xc4ac5665UL,
        0xf4292244UL, 0x432aff97UL, 0xab9423a7UL, 0xfc93a039UL,
        0x655b59c3UL, 0x8f0ccc92UL, 0xffeff47dUL, 0x85845dd1UL,
        0x6fa87e4fUL, 0xfe2ce6e0UL, 0xa3014314UL, 0x4e0811a1UL,
        0xf7537e82UL, 0xbd3af235UL, 0x2ad7d2bbUL, 0xeb86d391UL,
    };
    static const uint8_t sh[4][4] = {
        { 7, 12, 17, 22 }, { 5, 9, 14, 20 }, { 4, 11, 16, 23 }, { 6, 10, 15, 21 }
    };
    uint32_t w[16], a = s[0], b = s[1], c = s[2], d = s[3], f, t;
    int i, g;

    load_words(w, p);
    for (i = 0; i < 64; i++) {
        switch (i >> 4) {
        case 0:
            f = (b & c) | (~b & d);
            g = i;
            break;
        case 1:
            f = (d & b) | (~d & c);
            g = (5 * i + 1) & 15;
            break;
        case 2:
            f = b ^ c ^ d;
            g = (3 * i + 5) & 15;
            break;
        default:
            f = c ^ (b | ~d);
            g = (7 * i) & 15;
            break;
        }
        t = a + f + k[i] + w[g];
        a = d;
        d = c;
        c = b;
        b += ROL(t, sh[i >> 4][i & 3]);
    }
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
