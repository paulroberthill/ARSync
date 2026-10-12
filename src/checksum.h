/* checksum.h - delta-transfer checksums: block sizes, the rolling
 * checksum, and the strong (seeded MD5/MD4) block checksum */
#ifndef CHECKSUM_H
#define CHECKSUM_H

#include "arsync.h"

#define BLOCK_SIZE 700          /* minimum block length */
#define SUM_LENGTH 16           /* full strong checksum */
#define SHORT_SUM_LENGTH 2      /* minimum strong checksum in phase 1 */

typedef struct ars_sum_head {
    int32_t count;              /* number of blocks */
    int32_t blength;            /* block length */
    int32_t s2length;           /* bytes of strong checksum sent per block */
    int32_t remainder;          /* length of the last block, 0 if full */
} ars_sum_head;

/* Chooses block and checksum sizes for a basis file of len bytes, exactly
 * as rsync does (block length about sqrt(len), strong checksum long enough
 * to make false matches unlikely). min_s2length is 2 normally and 16 when
 * redoing a file that failed verification. Returns -1 if len is too big. */
int sum_sizes(int64_t len, int min_s2length, ars_sum_head *sh);

/* The weak checksum rsync calls checksum1: two 16-bit sums over the bytes
 * taken as *signed* chars. */
uint32_t rolling_sum(const uint8_t *buf, size_t len);

/* The strong block checksum (full 16 bytes; the first s2length are sent).
 * The seed is mixed in as four little-endian bytes, before the data when
 * seed_first (the CF_CHKSUM_SEED_FIX order), else after; not at all if 0. */
void block_sum(int digest_type, int32_t seed, int seed_first,
               const uint8_t *buf, size_t len, uint8_t out[SUM_LENGTH]);

#endif
