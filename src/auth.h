/* auth.h - rsync daemon authentication */
#ifndef AUTH_H
#define AUTH_H

#include "arsync.h"

/* Encodes len bytes as base64 without '=' padding, as rsync does. out needs
 * (len * 4 + 2) / 3 + 1 bytes. */
void base64_nopad(const uint8_t *in, size_t len, char *out);

/* Picks the first digest from ours (space-separated, preference order) that
 * also appears in theirs. Returns a DIGEST_* type, or 0 if none match. */
int choose_digest(const char *ours, const char *theirs);

/* The challenge response: base64(digest(password + challenge)).
 * out needs at least 32 bytes. */
void auth_response(int digest_type, const char *password, const char *challenge, char *out);

/* Reads the first line of a password file (CR/LF stripped). Returns 0, or
 * -1 if the file can't be read or holds no password. */
int read_password_file(const char *path, char *buf, size_t bufsize);

#endif
