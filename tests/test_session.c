/* test_session.c - locations, options, greeting/version negotiation, connection pump */
#include <stdlib.h>

#include "test.h"
#include "options.h"
#include "session.h"
#include "url.h"

static void test_parse_remote(void)
{
    ars_remote r;
    char err[128], mod[64];

    CHECK_EQ(parse_remote("rsync://example.org/", &r, err, sizeof err), 1);
    CHECK(strcmp(r.host, "example.org") == 0);
    CHECK_EQ(r.port, 0);
    CHECK(r.user[0] == '\0');
    CHECK(r.path[0] == '\0');

    CHECK_EQ(parse_remote("rsync://example.org", &r, err, sizeof err), 1);
    CHECK(strcmp(r.host, "example.org") == 0);
    CHECK(r.path[0] == '\0');

    CHECK_EQ(parse_remote("rsync://paul@10.0.0.1:8730/pub/dir/file", &r, err, sizeof err), 1);
    CHECK(strcmp(r.user, "paul") == 0);
    CHECK(strcmp(r.host, "10.0.0.1") == 0);
    CHECK_EQ(r.port, 8730);
    CHECK(strcmp(r.path, "pub/dir/file") == 0);
    remote_module(&r, mod, sizeof mod);
    CHECK(strcmp(mod, "pub") == 0);

    CHECK_EQ(parse_remote("host::", &r, err, sizeof err), 1);
    CHECK(strcmp(r.host, "host") == 0);
    CHECK(r.path[0] == '\0');

    CHECK_EQ(parse_remote("me@host::mod/a/b", &r, err, sizeof err), 1);
    CHECK(strcmp(r.user, "me") == 0);
    CHECK(strcmp(r.host, "host") == 0);
    CHECK(strcmp(r.path, "mod/a/b") == 0);
    remote_module(&r, mod, sizeof mod);
    CHECK(strcmp(mod, "mod") == 0);

    /* Local paths, including AmigaDOS ones with a single colon. */
    CHECK_EQ(parse_remote("Work:dest", &r, err, sizeof err), 0);
    CHECK_EQ(parse_remote("RAM:", &r, err, sizeof err), 0);
    CHECK_EQ(parse_remote("dir/sub", &r, err, sizeof err), 0);
    CHECK_EQ(parse_remote("dir/a::b", &r, err, sizeof err), 0);
    CHECK_EQ(parse_remote("::mod", &r, err, sizeof err), 0);

    /* Malformed daemon locations. */
    CHECK_EQ(parse_remote("rsync://", &r, err, sizeof err), -1);
    CHECK_EQ(parse_remote("rsync://host:/", &r, err, sizeof err), -1);
    CHECK_EQ(parse_remote("rsync://host:0/", &r, err, sizeof err), -1);
    CHECK_EQ(parse_remote("rsync://host:65536/", &r, err, sizeof err), -1);
    CHECK_EQ(parse_remote("rsync://host:8x/", &r, err, sizeof err), -1);
    CHECK_EQ(parse_remote("rsync://[::1]/", &r, err, sizeof err), -1);
    CHECK_EQ(parse_remote("user@::mod", &r, err, sizeof err), -1);
}

static void test_options(void)
{
    char *argv1[] = { "arsync", "-vv", "--port=8730", "--timeout", "30", "--no-motd",
                      "rsync://h/", "--", "-dest", NULL };
    char *argv2[] = { "arsync", "--port", NULL };
    char *argv3[] = { "arsync", "-x", NULL };
    char *argv4[] = { "arsync", "--port=99999", NULL };
    char *argv5[] = { "arsync", "a", "b", "c", NULL };
    char *argv6[] = { "arsync", "--verbose=2", NULL };
    char *argv7[] = { "arsync", "-", NULL };
    char *argv8[] = { "arsync", "-an", "--times", NULL };
    char err[128];
    ars_options o;

    CHECK_EQ(options_parse(&o, 9, argv1, err, sizeof err), 0);
    CHECK_EQ(o.verbose, 2);
    CHECK_EQ(o.port, 8730);
    CHECK_EQ(o.timeout, 30);
    CHECK_EQ(o.no_motd, 1);
    CHECK_EQ(o.nargs, 2);
    CHECK(strcmp(o.args[0], "rsync://h/") == 0);
    CHECK(strcmp(o.args[1], "-dest") == 0);

    CHECK_EQ(options_parse(&o, 2, argv2, err, sizeof err), -1);
    CHECK_EQ(options_parse(&o, 2, argv3, err, sizeof err), -1);
    CHECK_EQ(options_parse(&o, 2, argv4, err, sizeof err), -1);
    CHECK_EQ(options_parse(&o, 4, argv5, err, sizeof err), -1);
    CHECK_EQ(options_parse(&o, 2, argv6, err, sizeof err), -1);
    /* A lone "-" is an argument, as in rsync. */
    CHECK_EQ(options_parse(&o, 2, argv7, err, sizeof err), 0);
    CHECK_EQ(o.nargs, 1);
    /* -a means -rt for arsync (the rest of rsync's -a isn't supported). */
    CHECK_EQ(options_parse(&o, 3, argv8, err, sizeof err), 0);
    CHECK(o.recursive && o.times && o.dry_run);
}

static void test_greeting(void)
{
    char digests[64];
    int proto, sub;

    CHECK_EQ(parse_greeting("@RSYNCD: 32.0 sha512 sha256 sha1 md5 md4", &proto, &sub,
                            digests, sizeof digests), 0);
    CHECK_EQ(proto, 32);
    CHECK_EQ(sub, 0);
    CHECK(strcmp(digests, "sha512 sha256 sha1 md5 md4") == 0);

    CHECK_EQ(parse_greeting("@RSYNCD: 31.0", &proto, &sub, digests, sizeof digests), 0);
    CHECK_EQ(proto, 31);
    CHECK_EQ(sub, 0);
    CHECK(digests[0] == '\0');

    CHECK_EQ(parse_greeting("@RSYNCD: 29", &proto, &sub, digests, sizeof digests), 0);
    CHECK_EQ(proto, 29);
    CHECK_EQ(sub, -1);

    CHECK_EQ(parse_greeting("@RSYNCD: x", &proto, &sub, digests, sizeof digests), -1);
    CHECK_EQ(parse_greeting("@RSYNCD: 31.", &proto, &sub, digests, sizeof digests), -1);
    CHECK_EQ(parse_greeting("@RSYNCD: 31.0x", &proto, &sub, digests, sizeof digests), -1);
    CHECK_EQ(parse_greeting("@ERROR: go away", &proto, &sub, digests, sizeof digests), -1);
}

static void test_negotiation(void)
{
    CHECK_EQ(negotiate_protocol(32, 0, 32, 0), 32);
    CHECK_EQ(negotiate_protocol(32, 0, 31, 0), 31);
    CHECK_EQ(negotiate_protocol(32, 0, 30, 0), 30);
    /* A server on a newer release just speaks ours. */
    CHECK_EQ(negotiate_protocol(32, 0, 33, 0), 32);
    /* Pre-release subprotocols drop a version. */
    CHECK_EQ(negotiate_protocol(32, 0, 31, 2), 30);
    CHECK_EQ(negotiate_protocol(32, 0, 32, 1), 31);
}

#ifndef __amigaos__
/* Both ends write 512 KiB before reading anything. With plain blocking
 * writes this deadlocks once the socket buffers fill; the connection's pump
 * must keep accepting input while it waits to send. */
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include "net.h"
#include "plat.h"

enum { BULK = 512 * 1024 };

static void peer(int lsock)
{
    char *buf = malloc(BULK);
    int s = accept(lsock, NULL, NULL);
    size_t done;
    ssize_t n;

    memset(buf, 'S', BULK);
    for (done = 0; done < BULK; done += (size_t)n)
        if ((n = write(s, buf + done, BULK - done)) <= 0)
            _exit(1);
    for (done = 0; done < BULK; done += (size_t)n) {
        if ((n = read(s, buf, BULK)) <= 0)
            _exit(2);
        if (memchr(buf, 'C', (size_t)n) != buf || buf[n - 1] != 'C')
            _exit(3);
    }
    close(s);
    _exit(0);
}

static void test_no_deadlock(void)
{
    struct sockaddr_in sin;
    socklen_t slen = sizeof sin;
    ars_conn c;
    char *buf = malloc(BULK);
    int lsock, status;
    pid_t pid;

    lsock = socket(AF_INET, SOCK_STREAM, 0);
    memset(&sin, 0, sizeof sin);
    sin.sin_family = AF_INET;
    sin.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    bind(lsock, (struct sockaddr *)&sin, sizeof sin);
    listen(lsock, 1);
    getsockname(lsock, (struct sockaddr *)&sin, &slen);
    if ((pid = fork()) == 0)
        peer(lsock);
    close(lsock);

    CHECK_EQ(conn_open(&c, "127.0.0.1", ntohs(sin.sin_port), 10), PLAT_OK);
    memset(buf, 'C', BULK);
    w_bytes(&c.w, buf, BULK);
    CHECK_EQ(conn_flush(&c), WIRE_OK);
    memset(buf, 0, BULK);
    r_bytes(&c.r, buf, BULK);
    CHECK_EQ(c.r.err, WIRE_OK);
    CHECK(buf[0] == 'S' && buf[BULK - 1] == 'S');
    /* Then the peer hangs up: a clean EOF. */
    r_byte(&c.r);
    CHECK_EQ(c.r.err, WIRE_EOF);
    conn_close(&c);

    waitpid(pid, &status, 0);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    free(buf);
}
#endif

void test_session(void)
{
    test_parse_remote();
    test_options();
    test_greeting();
    test_negotiation();
#ifndef __amigaos__
    test_no_deadlock();
#endif
}
