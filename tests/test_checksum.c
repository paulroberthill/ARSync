/* test_checksum.c - delta checksums against rsync's own values */
#include "test.h"
#include "checksum.h"
#include "digest.h"
#include "sum_vectors.h"

#define COUNT(a) (sizeof(a) / sizeof((a)[0]))

static void hex16(const uint8_t *in, char *out)
{
    static const char digits[] = "0123456789abcdef";
    int i;

    for (i = 0; i < 16; i++) {
        out[2 * i] = digits[in[i] >> 4];
        out[2 * i + 1] = digits[in[i] & 15];
    }
    out[32] = '\0';
}

void test_checksum_suite(void)
{
    uint8_t buf[1000], sum[SUM_LENGTH];
    char got[33];
    ars_sum_head sh;
    size_t i;

    for (i = 0; i < sizeof buf; i++)
        buf[i] = (uint8_t)(i * 37 + 11);

    /* Weak checksum: bytes >= 0x80 count as negative, as in rsync. */
    for (i = 0; i < COUNT(sum1_vec); i++)
        CHECK_EQ(rolling_sum(buf, (size_t)sum1_vec[i].len), sum1_vec[i].sum1);

    for (i = 0; i < COUNT(sizes_vec); i++) {
        int64_t blocks = sizes_vec[i].len / sizes_vec[i].blength
                         + (sizes_vec[i].len % sizes_vec[i].blength != 0);

        /* Too many blocks for the protocol's 32-bit count: refused, as rsync does. */
        if (blocks > 0x7FFFFFFF) {
            CHECK_EQ(sum_sizes(sizes_vec[i].len, sizes_vec[i].csum_length, &sh), -1);
            continue;
        }
        CHECK_EQ(sum_sizes(sizes_vec[i].len, sizes_vec[i].csum_length, &sh), 0);
        CHECK_EQ(sh.blength, sizes_vec[i].blength);
        CHECK_EQ(sh.s2length, sizes_vec[i].s2length);
        CHECK_EQ(sh.remainder, sizes_vec[i].len % sizes_vec[i].blength);
        CHECK_EQ(sh.count, blocks);
    }
    CHECK_EQ(sum_sizes(-1, 2, &sh), -1);

    /* Strong block checksum and the seed's position (checked with
     * Python's hashlib; seed -123456789 as little-endian bytes). */
    block_sum(DIGEST_MD5, -123456789, 1, buf, sizeof buf, sum);
    hex16(sum, got);
    CHECK(strcmp(got, "3a2b566cd596f9e38de9f3ef59dfa17b") == 0);
    block_sum(DIGEST_MD5, -123456789, 0, buf, sizeof buf, sum);
    hex16(sum, got);
    CHECK(strcmp(got, "21928eba194648382d939acb6f3b0d5f") == 0);
    block_sum(DIGEST_MD5, 0, 1, buf, sizeof buf, sum);    /* seed 0: not mixed in */
    hex16(sum, got);
    CHECK(strcmp(got, "c3cb8ae118768782aa1896d7fb2827ef") == 0);
}
