/* match.h - the sending side of a delta: find the receiver's blocks in a
 * local file and send the file as literal data plus block references */
#ifndef MATCH_H
#define MATCH_H

#include "checksum.h"
#include "plat.h"
#include "wire.h"

typedef struct ars_sums {
    ars_sum_head h;
    uint32_t *sum1;         /* weak checksum per block */
    uint8_t *sum2;          /* h.s2length bytes of strong checksum per block */
    int32_t *head;          /* hash table of block indexes by weak checksum */
    int32_t *next;
    uint32_t mask;
} ars_sums;

/* Reads the receiver's checksum header and block checksums. Returns
 * WIRE_OK, a stream error, WIRE_EPROTO (bad header) or WIRE_ENOMEM. */
int sums_read(ars_sums *s, ars_reader *r);
void sums_free(ars_sums *s);

typedef struct ars_match_ctx {
    int digest_type;
    int32_t seed;
    int seed_first;
    uint8_t *buf;           /* MATCH_BUFSIZE bytes */
    void (*progress)(void *ctx, int64_t done);
    void *progress_ctx;
    int64_t literal;        /* statistics: bytes sent as data */
    int64_t matched;        /* bytes covered by block references */
    int read_error;         /* the file couldn't be read in full */
} ars_match_ctx;

#define MATCH_BUFSIZE (CHUNK_BUFSIZE + 2 * 131072)
#define CHUNK_BUFSIZE 32768

/* Sends file f as tokens (ending with 0) followed by its whole-file
 * checksum. Returns WIRE_OK, a stream error, or WIRE_EABORT on Ctrl-C. */
int send_delta(ars_match_ctx *m, plat_file *f, const ars_sums *s, ars_writer *w);

#endif
