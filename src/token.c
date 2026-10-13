/* token.c - the file-data token stream, plain or compressed (-z)
 *
 * Plain: int32 n > 0 then n literal bytes; int32 -(i + 1) for block i of
 * the receiver's old file; int32 0 at the end.
 *
 * Compressed, as rsync's token.c defines it: literal data goes through one
 * raw deflate stream per file, cut into records, and tokens become flag
 * bytes:
 *
 *   0x00                   end of file
 *   0x40 | hi6, lo8        deflated data follows (14-bit length)
 *   0x80 | r               block (last block of the previous run + r), r < 64
 *   0xC0 | r, n16          a run: that block and the n after it
 *   0x20, int32            block given in full
 *   0x21, int32, n16       a run, given in full
 *
 * Before each token the compressor is flushed (Z_SYNC_FLUSH) and the
 * 00 00 FF FF that ends the flush is left out; the receiver puts it back.
 * With "zlib" (but not "zlibx") the data of every matched block also goes
 * into both sides' compression history, as if it had been sent: the
 * receiver inflates it wrapped in a stored-block header, the sender (where
 * rsync uses its own zlib's Z_INSERT_ONLY) adds it as a dictionary, which
 * a raw deflate stream allows after a flush.
 */
#include <stdlib.h>
#include <string.h>

#include "token.h"
#include "options.h"

#define END_FLAG 0x00
#define TOKEN_LONG 0x20
#define TOKENRUN_LONG 0x21
#define DEFLATED_DATA 0x40
#define TOKEN_REL 0x80
#define TOKENRUN_REL 0xC0

#define MAX_DATA_COUNT 16383        /* longest deflated-data record */
/* Enough output space to inflate a record (or deflate a chunk) in one go:
 * rsync's AVAIL_OUT_SIZE(CHUNK_SIZE). */
#define DBUF_SIZE (TOKEN_CHUNK * 1001 / 1000 + 16)
#define OBUF_SIZE 65536

enum { R_INIT, R_IDLE, R_RUNNING, R_INFLATING, R_INFLATED };

/* --- receiving --- */

int token_rx_init(ars_token_rx *t, int compression)
{
    memset(t, 0, sizeof *t);
    t->compression = compression;
    t->buf = malloc(DBUF_SIZE);
    if (compression)
        t->cbuf = malloc(MAX_DATA_COUNT);
    if (!t->buf || (compression && !t->cbuf)) {
        token_rx_free(t);
        return WIRE_ENOMEM;
    }
    return WIRE_OK;
}

void token_rx_free(ars_token_rx *t)
{
    if (t->z_ready)
        inflateEnd(&t->z);
    free(t->buf);
    free(t->cbuf);
    memset(t, 0, sizeof *t);
}

static int32_t plain_recv(ars_token_rx *t, ars_reader *r, const uint8_t **data, int *err)
{
    int32_t n;

    if (t->literal_left == 0) {
        n = r_int32(r);
        if (r->err) {
            *err = r->err;
            return 0;
        }
        if (n <= 0)
            return n;
        t->literal_left = n;
    }
    n = t->literal_left > TOKEN_CHUNK ? TOKEN_CHUNK : t->literal_left;
    r_bytes(r, t->buf, (size_t)n);
    if (r->err) {
        *err = r->err;
        return 0;
    }
    t->literal_left -= n;
    *data = t->buf;
    return n;
}

static int32_t fail(int *err, int code)
{
    *err = code;
    return 0;
}

int32_t token_recv(ars_token_rx *t, ars_reader *r, const uint8_t **data, int *err)
{
    int32_t flag, n;
    int zr;

    *err = WIRE_OK;
    if (!t->compression)
        return plain_recv(t, r, data, err);

    for (;;) {
        switch (t->state) {
        case R_INIT:
            if (!t->z_ready) {
                if (inflateInit2(&t->z, -15) != Z_OK)
                    return fail(err, WIRE_ENOMEM);
                t->z_ready = 1;
            } else {
                inflateReset(&t->z);
            }
            t->state = R_IDLE;
            t->token = 0;
            break;

        case R_IDLE:
        case R_INFLATED:
            if (t->saved_flag) {
                flag = t->saved_flag & 0xFF;
                t->saved_flag = 0;
            } else {
                flag = r_byte(r);
            }
            if (r->err)
                return fail(err, r->err);
            if ((flag & 0xC0) == DEFLATED_DATA) {
                n = ((flag & 0x3F) << 8) + r_byte(r);
                r_bytes(r, t->cbuf, (size_t)n);
                if (r->err)
                    return fail(err, r->err);
                t->z.next_in = t->cbuf;
                t->z.avail_in = (uInt)n;
                t->state = R_INFLATING;
                break;
            }
            if (t->state == R_INFLATED) {
                /* The data before this token is complete: collect what
                 * the decompressor still holds, then give it the flush
                 * marker the sender left out. */
                t->z.avail_in = 0;
                t->z.next_out = t->buf;
                t->z.avail_out = DBUF_SIZE;
                zr = inflate(&t->z, Z_SYNC_FLUSH);
                n = (int32_t)(DBUF_SIZE - t->z.avail_out);
                if (zr != Z_OK && zr != Z_BUF_ERROR)
                    return fail(err, WIRE_EPROTO);
                if (n != 0 && zr != Z_BUF_ERROR) {
                    t->saved_flag = flag + 0x10000;
                    *data = t->buf;
                    return n;
                }
                if (!inflateSyncPoint(&t->z))
                    return fail(err, WIRE_EPROTO);
                t->cbuf[0] = t->cbuf[1] = 0;
                t->cbuf[2] = t->cbuf[3] = 0xFF;
                t->z.next_in = t->cbuf;
                t->z.avail_in = 4;
                inflate(&t->z, Z_SYNC_FLUSH);
                t->state = R_IDLE;
            }
            if (flag == END_FLAG) {
                t->state = R_INIT;
                return 0;
            }
            if (flag & TOKEN_REL) {
                t->token += flag & 0x3F;
                flag >>= 6;
            } else {
                t->token = r_int32(r);
            }
            if (flag & 1) {
                t->run = r_byte(r);
                t->run += r_byte(r) << 8;
                t->state = R_RUNNING;
            }
            if (r->err)
                return fail(err, r->err);
            return -1 - t->token;

        case R_INFLATING:
            t->z.next_out = t->buf;
            t->z.avail_out = DBUF_SIZE;
            zr = inflate(&t->z, Z_NO_FLUSH);
            n = (int32_t)(DBUF_SIZE - t->z.avail_out);
            if (zr != Z_OK)
                return fail(err, WIRE_EPROTO);
            if (t->z.avail_in == 0)
                t->state = R_INFLATED;
            if (n != 0) {
                *data = t->buf;
                return n;
            }
            break;

        case R_RUNNING:
            t->token++;
            if (--t->run == 0)
                t->state = R_IDLE;
            return -1 - t->token;
        }
    }
}

int token_see(ars_token_rx *t, const uint8_t *data, int32_t len)
{
    uint8_t hdr[5];
    int32_t blklen = 0;
    int zr;

    if (t->compression != CPRES_ZLIB || len <= 0)
        return WIRE_OK;
    /* Inflate the block as stored blocks (at most 64 KiB each), so it
     * enters the history window. (rsync before protocol 31 re-fed the
     * first 64 KiB of longer blocks; blocks never get that big for files
     * AmigaDOS can hold.) */
    t->z.avail_in = 0;
    hdr[0] = 0;
    do {
        if (t->z.avail_in == 0 && len != 0) {
            if (blklen == 0) {
                blklen = len > 0xFFFF ? 0xFFFF : len;
                hdr[1] = (uint8_t)blklen;
                hdr[2] = (uint8_t)(blklen >> 8);
                hdr[3] = (uint8_t)~hdr[1];
                hdr[4] = (uint8_t)~hdr[2];
                t->z.next_in = hdr;
                t->z.avail_in = 5;
            } else {
                t->z.next_in = (Bytef *)data;
                t->z.avail_in = (uInt)blklen;
                data += blklen;
                len -= blklen;
                blklen = 0;
            }
        }
        t->z.next_out = t->buf;
        t->z.avail_out = DBUF_SIZE;
        zr = inflate(&t->z, Z_SYNC_FLUSH);
        if (zr != Z_OK && zr != Z_BUF_ERROR)
            return WIRE_EPROTO;
    } while (len || t->z.avail_out == 0);
    return WIRE_OK;
}

/* --- sending --- */

int token_tx_init(ars_token_tx *t, int compression)
{
    memset(t, 0, sizeof *t);
    t->compression = compression;
    t->level = Z_DEFAULT_COMPRESSION;
    t->last_token = -1;
    if (compression && !(t->obuf = malloc(OBUF_SIZE)))
        return WIRE_ENOMEM;
    return WIRE_OK;
}

void token_tx_free(ars_token_tx *t)
{
    if (t->z_ready)
        deflateEnd(&t->z);
    free(t->obuf);
    memset(t, 0, sizeof *t);
}

void token_tx_level(ars_token_tx *t, int level)
{
    t->level = level;
}

/* Sends k bytes of compressed output as one record. */
static void emit(ars_token_tx *t, ars_writer *w, size_t k)
{
    w_byte(w, (uint8_t)(DEFLATED_DATA + (k >> 8)));
    w_byte(w, (uint8_t)k);
    w_bytes(w, t->obuf, k);
    t->olen -= k;
    memmove(t->obuf, t->obuf + k, t->olen);
}

static int deflate_data(ars_token_tx *t, ars_writer *w, const uint8_t *data, size_t n, int flush)
{
    int zr, full;

    t->z.next_in = (Bytef *)data;
    t->z.avail_in = (uInt)n;
    for (;;) {
        t->z.next_out = t->obuf + t->olen;
        t->z.avail_out = (uInt)(OBUF_SIZE - t->olen);
        zr = deflate(&t->z, flush);
        if (zr != Z_OK && zr != Z_BUF_ERROR)
            return WIRE_EPROTO;
        full = t->z.avail_out == 0;
        t->olen = OBUF_SIZE - t->z.avail_out;
        /* Hold back the last 4 bytes: they may be the flush marker. */
        while (t->olen >= MAX_DATA_COUNT + 4)
            emit(t, w, MAX_DATA_COUNT);
        if (t->z.avail_in == 0 && !full)
            break;
    }
    if (flush == Z_SYNC_FLUSH) {
        if (t->olen < 4)
            return WIRE_EPROTO;
        t->olen -= 4;       /* 00 00 FF FF */
        while (t->olen > 0)
            emit(t, w, t->olen > MAX_DATA_COUNT ? MAX_DATA_COUNT : t->olen);
    }
    return WIRE_OK;
}

static int start_file(ars_token_tx *t)
{
    if (t->z_ready && t->z_level != t->level) {
        deflateEnd(&t->z);
        t->z_ready = 0;
    }
    if (!t->z_ready) {
        memset(&t->z, 0, sizeof t->z);
        if (deflateInit2(&t->z, t->level, Z_DEFLATED, -15, 8, Z_DEFAULT_STRATEGY) != Z_OK)
            return WIRE_ENOMEM;
        t->z_ready = 1;
        t->z_level = t->level;
    } else {
        deflateReset(&t->z);
    }
    t->olen = 0;
    return WIRE_OK;
}

int token_send(ars_token_tx *t, ars_writer *w, int32_t token, const uint8_t *data, size_t n,
               const uint8_t *block, size_t blen)
{
    int32_t r, k;
    size_t chunk;
    uInt piece;
    int rc;

    if (!t->compression) {
        while (n > 0) {
            chunk = n > TOKEN_CHUNK ? TOKEN_CHUNK : n;
            w_int32(w, (int32_t)chunk);
            w_bytes(w, data, chunk);
            data += chunk;
            n -= chunk;
        }
        if (token != -2)
            w_int32(w, -(token + 1));
        return w->err;
    }

    /* Tokens are sent a run at a time, once the run is known to end. */
    if (t->last_token == -1) {
        if ((rc = start_file(t)) != WIRE_OK)
            return rc;
        t->last_run_end = 0;
        t->run_start = token;
        t->flush_pending = 0;
    } else if (t->last_token == -2) {
        t->run_start = token;
    } else if (n != 0 || token != t->last_token + 1 || token >= t->run_start + 65536) {
        r = t->run_start - t->last_run_end;
        k = t->last_token - t->run_start;
        if (r >= 0 && r <= 63) {
            w_byte(w, (uint8_t)((k == 0 ? TOKEN_REL : TOKENRUN_REL) + r));
        } else {
            w_byte(w, k == 0 ? TOKEN_LONG : TOKENRUN_LONG);
            w_int32(w, t->run_start);
        }
        if (k != 0) {
            w_byte(w, (uint8_t)k);
            w_byte(w, (uint8_t)(k >> 8));
        }
        t->last_run_end = t->last_token;
        t->run_start = token;
    }
    t->last_token = token;

    if (n != 0 || t->flush_pending) {
        /* Data before a token is flushed, so the token can follow it;
         * data alone (-2) can stay in the compressor. */
        if ((rc = deflate_data(t, w, data, n, token == -2 ? Z_NO_FLUSH : Z_SYNC_FLUSH)) != WIRE_OK)
            return rc;
        t->flush_pending = token == -2;
    }

    if (token == -1) {
        w_byte(w, END_FLAG);
    } else if (token >= 0 && t->compression == CPRES_ZLIB) {
        while (blen > 0) {
            piece = blen > 0xFFFF ? 0xFFFF : (uInt)blen;
            if (deflateSetDictionary(&t->z, block, piece) != Z_OK)
                return WIRE_EPROTO;
            block += piece;
            blen -= piece;
        }
    }
    return w->err;
}
