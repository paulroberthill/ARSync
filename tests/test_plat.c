/* test_plat.c - platform layer: files, directories, times (and sockets on the host)
 *
 * Works in a scratch directory relative to the current directory, so it runs
 * the same on the host and under vamos.
 */
#include <stdlib.h>

#include "test.h"
#include "plat.h"

#define TDIR "arsync-test.tmp"

static void cleanup(void)
{
    plat_remove(TDIR "/a");
    plat_remove(TDIR "/b");
    plat_remove(TDIR "/c");
    plat_remove(TDIR);
}

static void test_files(void)
{
    enum { SIZE = 3000 };
    uint8_t out[SIZE], in[SIZE + 10];
    plat_file *f;
    plat_fileinfo st;
    plat_dir *d;
    char name[108];
    int i, n, seen_c;
    long got;

    for (i = 0; i < SIZE; i++)
        out[i] = (uint8_t)(i * 13 + 1);

    cleanup();
    CHECK_EQ(plat_stat(TDIR, &st), PLAT_ENOENT);
    CHECK_EQ(plat_mkdir(TDIR), PLAT_OK);
    CHECK_EQ(plat_mkdir(TDIR), PLAT_EEXIST);
    CHECK_EQ(plat_stat(TDIR, &st), PLAT_OK);
    CHECK_EQ(st.type, PLAT_T_DIR);

    /* Write, stat, read back. */
    CHECK_EQ(plat_open_write(TDIR "/a", &f), PLAT_OK);
    CHECK_EQ(plat_write(f, out, 1000), PLAT_OK);
    CHECK_EQ(plat_write(f, out + 1000, SIZE - 1000), PLAT_OK);
    CHECK_EQ(plat_close(f), PLAT_OK);

    CHECK_EQ(plat_stat(TDIR "/a", &st), PLAT_OK);
    CHECK_EQ(st.type, PLAT_T_FILE);
    CHECK_EQ(st.size, SIZE);

    CHECK_EQ(plat_open_read(TDIR "/a", &f), PLAT_OK);
    got = 0;
    while ((n = (int)plat_read(f, in + got, sizeof in - (size_t)got)) > 0)
        got += n;
    CHECK_EQ(n, 0);
    CHECK_EQ(got, SIZE);
    CHECK_MEM(in, out, SIZE);
    CHECK_EQ(plat_seek(f, 2000), PLAT_OK);
    CHECK_EQ(plat_read(f, in, 10), 10);
    CHECK_MEM(in, out + 2000, 10);
#ifdef __amigaos__
    CHECK_EQ(plat_seek(f, (int64_t)1 << 40), PLAT_ETOOBIG);   /* OS3 offsets are 32-bit */
#endif
    CHECK_EQ(plat_close(f), PLAT_OK);
    CHECK_EQ(plat_open_read(TDIR "/missing", &f), PLAT_ENOENT);

    /* Modification time round-trips at whole-second precision everywhere
     * (AmigaDOS keeps 1/50 s). */
    CHECK_EQ(plat_set_mtime(TDIR "/a", 1700000000, 0), PLAT_OK);
    CHECK_EQ(plat_stat(TDIR "/a", &st), PLAT_OK);
    CHECK_EQ(st.mtime, 1700000000);

    /* Rename, including over an existing file. */
    CHECK_EQ(plat_rename(TDIR "/a", TDIR "/b"), PLAT_OK);
    CHECK_EQ(plat_stat(TDIR "/a", &st), PLAT_ENOENT);
    CHECK_EQ(plat_open_write(TDIR "/c", &f), PLAT_OK);
    CHECK_EQ(plat_close(f), PLAT_OK);
    CHECK_EQ(plat_rename(TDIR "/b", TDIR "/c"), PLAT_OK);
    CHECK_EQ(plat_stat(TDIR "/c", &st), PLAT_OK);
    CHECK_EQ(st.size, SIZE);

    /* Directory listing: exactly one entry, "c". */
    CHECK_EQ(plat_opendir(TDIR, &d), PLAT_OK);
    seen_c = 0;
    n = 0;
    while ((i = plat_readdir(d, name, sizeof name)) == 1) {
        n++;
        if (strcmp(name, "c") == 0)
            seen_c = 1;
    }
    CHECK_EQ(i, 0);
    CHECK_EQ(n, 1);
    CHECK(seen_c);
    plat_closedir(d);
    CHECK_EQ(plat_opendir(TDIR "/c", &d), PLAT_ENOENT);   /* a file, not a directory */

    /* Removal: a non-empty directory refuses, then everything goes. */
    CHECK(plat_remove(TDIR) != PLAT_OK);
    CHECK_EQ(plat_remove(TDIR "/c"), PLAT_OK);
    CHECK_EQ(plat_remove(TDIR "/c"), PLAT_ENOENT);
    CHECK_EQ(plat_remove(TDIR), PLAT_OK);
}

#ifndef __amigaos__
/* Loopback round trip through the non-blocking socket calls (host only:
 * vamos has no TCP/IP stack). */
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

static void test_sockets(void)
{
    struct sockaddr_in sin;
    socklen_t slen = sizeof sin;
    char buf[8];
    int lsock, csock, ssock, rd, wr;

    lsock = socket(AF_INET, SOCK_STREAM, 0);
    memset(&sin, 0, sizeof sin);
    sin.sin_family = AF_INET;
    sin.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    CHECK(bind(lsock, (struct sockaddr *)&sin, sizeof sin) == 0);
    CHECK(listen(lsock, 1) == 0);
    CHECK(getsockname(lsock, (struct sockaddr *)&sin, &slen) == 0);

    CHECK_EQ(plat_tcp_connect("127.0.0.1", ntohs(sin.sin_port), &csock), PLAT_OK);
    ssock = accept(lsock, NULL, NULL);
    CHECK(ssock >= 0);

    /* Nothing to read yet: non-blocking recv and a short wait both say so. */
    CHECK_EQ(plat_sock_recv(csock, buf, sizeof buf), PLAT_EAGAIN);
    CHECK_EQ(plat_sock_wait(csock, 1, 0, 50, &rd, &wr), PLAT_ETIMEOUT);

    CHECK_EQ(plat_sock_send(csock, "ping", 4), 4);
    CHECK_EQ(read(ssock, buf, 4), 4);
    CHECK_MEM(buf, "ping", 4);
    CHECK_EQ(write(ssock, "pong", 4), 4);
    CHECK_EQ(plat_sock_wait(csock, 1, 0, 1000, &rd, &wr), PLAT_OK);
    CHECK(rd);
    CHECK_EQ(plat_sock_recv(csock, buf, sizeof buf), 4);
    CHECK_MEM(buf, "pong", 4);

    /* Peer close reads as EOF. */
    close(ssock);
    CHECK_EQ(plat_sock_wait(csock, 1, 0, 1000, &rd, &wr), PLAT_OK);
    CHECK_EQ(plat_sock_recv(csock, buf, sizeof buf), 0);

    plat_sock_close(csock);
    close(lsock);

    CHECK_EQ(plat_tcp_connect("no-such-host.invalid", 873, &csock), PLAT_EHOST);
}
#endif

void test_plat(void)
{
    test_files();
#ifndef __amigaos__
    test_sockets();
#endif
}
