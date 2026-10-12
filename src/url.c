/* url.c - parsing rsync daemon locations */
#include <stdio.h>
#include <string.h>

#include "url.h"

static int copy_part(char *dst, size_t dstsize, const char *src, size_t len)
{
    if (len >= dstsize)
        return -1;
    memcpy(dst, src, len);
    dst[len] = '\0';
    return 0;
}

/* Parses "[USER@]HOST" from s[0..len) into rem. */
static int parse_userhost(const char *s, size_t len, ars_remote *rem, char *err, size_t errlen)
{
    const char *at = memchr(s, '@', len);

    if (at) {
        if (copy_part(rem->user, sizeof rem->user, s, (size_t)(at - s)) < 0) {
            snprintf(err, errlen, "user name too long");
            return -1;
        }
        len -= (size_t)(at + 1 - s);
        s = at + 1;
    }
    if (len == 0) {
        snprintf(err, errlen, "missing host name");
        return -1;
    }
    if (*s == '[') {
        snprintf(err, errlen, "IPv6 addresses are not supported");
        return -1;
    }
    if (copy_part(rem->host, sizeof rem->host, s, len) < 0) {
        snprintf(err, errlen, "host name too long");
        return -1;
    }
    return 0;
}

int parse_remote(const char *arg, ars_remote *rem, char *err, size_t errlen)
{
    const char *p, *slash, *colon, *end;
    long port;

    memset(rem, 0, sizeof *rem);

    if (strncmp(arg, "rsync://", 8) == 0) {
        p = arg + 8;
        slash = strchr(p, '/');
        end = slash ? slash : p + strlen(p);
        colon = memchr(p, ':', (size_t)(end - p));
        if (colon) {
            if (colon + 1 == end) {
                snprintf(err, errlen, "missing port number in %s", arg);
                return -1;
            }
            port = 0;
            for (p = colon + 1; p < end; p++) {
                if (*p < '0' || *p > '9' || (port = port * 10 + (*p - '0')) > 65535) {
                    snprintf(err, errlen, "bad port number in %s", arg);
                    return -1;
                }
            }
            if (port == 0) {
                snprintf(err, errlen, "bad port number in %s", arg);
                return -1;
            }
            rem->port = (int)port;
        }
        if (parse_userhost(arg + 8, (size_t)((colon ? colon : end) - (arg + 8)), rem, err, errlen) < 0)
            return -1;
        p = slash ? slash + 1 : end;
    } else if ((colon = strstr(arg, "::")) != NULL && colon > arg && !memchr(arg, '/', (size_t)(colon - arg))) {
        if (parse_userhost(arg, (size_t)(colon - arg), rem, err, errlen) < 0)
            return -1;
        p = colon + 2;
    } else {
        return 0;
    }

    if (copy_part(rem->path, sizeof rem->path, p, strlen(p)) < 0) {
        snprintf(err, errlen, "path too long");
        return -1;
    }
    return 1;
}

void remote_module(const ars_remote *rem, char *buf, size_t bufsize)
{
    size_t len = strcspn(rem->path, "/");

    if (len >= bufsize)
        len = bufsize - 1;
    memcpy(buf, rem->path, len);
    buf[len] = '\0';
}
