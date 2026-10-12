/* digest.h - MD4 (RFC 1320) and MD5 (RFC 1321) message digests */
#ifndef DIGEST_H
#define DIGEST_H

#include "arsync.h"

#define DIGEST_MD4 1
#define DIGEST_MD5 2
#define DIGEST_LEN 16   /* both produce 128-bit digests */

typedef struct ars_digest {
    int type;
    uint32_t state[4];
    uint8_t block[64];
    uint64_t total;     /* bytes hashed so far */
} ars_digest;

void digest_init(ars_digest *d, int type);
void digest_update(ars_digest *d, const void *data, size_t len);
void digest_final(ars_digest *d, uint8_t out[DIGEST_LEN]);

/* Maps "md4"/"md5" to a DIGEST_* type, or 0 if unknown. */
int digest_by_name(const char *name, size_t len);

#endif
