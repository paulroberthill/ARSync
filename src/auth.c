/* auth.c - rsync daemon authentication */
#include <string.h>

#include "auth.h"
#include "digest.h"
#include "plat.h"

void base64_nopad(const uint8_t *in, size_t len, char *out)
{
    static const char b64[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    uint32_t acc = 0;
    int bits = 0;

    while (len--) {
        acc = acc << 8 | *in++;
        bits += 8;
        while (bits >= 6) {
            bits -= 6;
            *out++ = b64[(acc >> bits) & 0x3F];
        }
    }
    if (bits > 0)
        *out++ = b64[(acc << (6 - bits)) & 0x3F];
    *out = '\0';
}

/* Does the space-separated list contain the word name[0..len)? */
static int list_has(const char *list, const char *name, size_t len)
{
    const char *p = list;
    size_t n;

    while (*p) {
        while (*p == ' ')
            p++;
        n = strcspn(p, " ");
        if (n == len && memcmp(p, name, len) == 0)
            return 1;
        p += n;
    }
    return 0;
}

int choose_digest(const char *ours, const char *theirs)
{
    const char *p = ours;
    size_t n;
    int type;

    while (*p) {
        while (*p == ' ')
            p++;
        n = strcspn(p, " ");
        if (n && (type = digest_by_name(p, n)) != 0 && list_has(theirs, p, n))
            return type;
        p += n;
    }
    return 0;
}

void auth_response(int digest_type, const char *password, const char *challenge, char *out)
{
    uint8_t sum[DIGEST_LEN];
    ars_digest d;

    digest_init(&d, digest_type);
    digest_update(&d, password, strlen(password));
    digest_update(&d, challenge, strlen(challenge));
    digest_final(&d, sum);
    base64_nopad(sum, sizeof sum, out);
}

int read_password_file(const char *path, char *buf, size_t bufsize)
{
    plat_file *f;
    long n;
    size_t len;

    if (plat_open_read(path, &f) != PLAT_OK)
        return -1;
    n = plat_read(f, buf, bufsize - 1);
    plat_close(f);
    if (n <= 0)
        return -1;
    buf[n] = '\0';
    /* Like rsync: the first run of characters up to CR or LF. */
    len = strspn(buf, "\r\n");
    memmove(buf, buf + len, (size_t)n - len + 1);
    buf[strcspn(buf, "\r\n")] = '\0';
    return buf[0] ? 0 : -1;
}
