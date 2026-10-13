/* token.h - the file-data token stream, plain or compressed (-z)
 *
 * A file's data travels as a sequence of literal data and references to
 * blocks of the receiver's old copy, then an end marker. Without
 * compression that's int32 tokens (n > 0: n literal bytes follow; -(i+1):
 * block i; 0: end). With -z the literal data is deflated and tokens are
 * packed into flag bytes; see token.c.
 */
#ifndef TOKEN_H
#define TOKEN_H

#include <zlib.h>

#include "wire.h"

#define TOKEN_CHUNK 32768           /* most literal data per plain token */

/* --- receiving --- */

typedef struct ars_token_rx {
    int compression;                /* CPRES_* */
    int32_t literal_left;           /* plain: bytes of the current token left */
    uint8_t *buf;                   /* decoded data handed out */
    /* compressed */
    z_stream z;
    int z_ready;
    int state;
    int32_t saved_flag;             /* a flag byte read too early, + 0x10000 */
    int32_t token, run;
    uint8_t *cbuf;                  /* compressed data as received */
} ars_token_rx;

int token_rx_init(ars_token_rx *t, int compression);  /* WIRE_OK or WIRE_ENOMEM */
void token_rx_free(ars_token_rx *t);

/* Reads the next piece of a file: returns n > 0 with n bytes of literal
 * data at *data (valid until the next call), -(i + 1) for block i, 0 at
 * the end of the file. On an error returns 0 with *err set to a WIRE_E*
 * code (r->err for stream errors, WIRE_EPROTO for bad compressed data). */
int32_t token_recv(ars_token_rx *t, ars_reader *r, const uint8_t **data, int *err);

/* Tells the decompressor about a matched block's data ("zlib" keeps it in
 * its history; "zlibx" and plain ignore it). Returns WIRE_OK or
 * WIRE_EPROTO. */
int token_see(ars_token_rx *t, const uint8_t *data, int32_t len);

/* --- sending --- */

typedef struct ars_token_tx {
    int compression;
    int level;                      /* zlib level for the next file */
    z_stream z;
    int z_ready;
    int z_level;                    /* level z was set up with */
    int32_t last_token, run_start, last_run_end;
    int flush_pending;
    uint8_t *obuf;
    size_t olen;
} ars_token_tx;

int token_tx_init(ars_token_tx *t, int compression);  /* WIRE_OK or WIRE_ENOMEM */
void token_tx_free(ars_token_tx *t);

/* Sends n literal bytes, then a token: i >= 0 for block i (whose data,
 * blen bytes, is at block), -1 to end the file, -2 for none (just data,
 * n > 0). Returns WIRE_OK, w->err, or WIRE_EPROTO if the compressor failed. */
int token_send(ars_token_tx *t, ars_writer *w, int32_t token, const uint8_t *data, size_t n,
               const uint8_t *block, size_t blen);
/* Compression level for the next file (0 = store, for already-packed
 * data; -1 = default). */
void token_tx_level(ars_token_tx *t, int level);

#endif
