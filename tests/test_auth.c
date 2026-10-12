/* test_auth.c - MD4/MD5, base64, daemon authentication, server arguments */
#include <stdlib.h>

#include "test.h"
#include "auth.h"
#include "digest.h"
#include "plat.h"
#include "session.h"

static void hex(const uint8_t *in, char *out)
{
    static const char digits[] = "0123456789abcdef";
    int i;

    for (i = 0; i < DIGEST_LEN; i++) {
        out[2 * i] = digits[in[i] >> 4];
        out[2 * i + 1] = digits[in[i] & 15];
    }
    out[2 * DIGEST_LEN] = '\0';
}

static int digest_is(int type, const char *msg, const char *want)
{
    uint8_t sum[DIGEST_LEN];
    char got[2 * DIGEST_LEN + 1];
    ars_digest d;

    digest_init(&d, type);
    digest_update(&d, msg, strlen(msg));
    digest_final(&d, sum);
    hex(sum, got);
    if (strcmp(got, want) != 0)
        printf("  %s(\"%s\") = %s, want %s\n", type == DIGEST_MD4 ? "md4" : "md5", msg, got, want);
    return strcmp(got, want) == 0;
}

/* Test suites from RFC 1320 (MD4) and RFC 1321 (MD5). */
static void test_digests(void)
{
    static const char *const msgs[] = {
        "", "a", "abc", "message digest", "abcdefghijklmnopqrstuvwxyz",
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789",
        "12345678901234567890123456789012345678901234567890123456789012345678901234567890",
    };
    static const char *const md4[] = {
        "31d6cfe0d16ae931b73c59d7e0c089c0", "bde52cb31de33e46245e05fbdbd6fb24",
        "a448017aaf21d8525fc10ae87aa6729d", "d9130a8164549fe818874806e1c7014b",
        "d79e1c308aa5bbcdeea8ed63df412da9", "043f8582f241db351ce627e153e7f0e4",
        "e33b4ddc9c38f2199c3e7b164fcc0536",
    };
    static const char *const md5[] = {
        "d41d8cd98f00b204e9800998ecf8427e", "0cc175b9c0f1b6a831c399e269772661",
        "900150983cd24fb0d6963f7d28e17f72", "f96b697d7cb7938d525a2f31aaf161d0",
        "c3fcd3d76192e4007dfb496cca67e13b", "d174ab98d277d9f5a5611c2c9f419d9f",
        "57edf4a22be3c955ac49da2e2107b67a",
    };
    uint8_t sum[DIGEST_LEN], *buf;
    char got[2 * DIGEST_LEN + 1];
    ars_digest d;
    size_t i, done, step;

    for (i = 0; i < sizeof msgs / sizeof msgs[0]; i++) {
        CHECK(digest_is(DIGEST_MD4, msgs[i], md4[i]));
        CHECK(digest_is(DIGEST_MD5, msgs[i], md5[i]));
    }

    /* A million 'a's fed in uneven pieces exercises the block buffering. */
    buf = malloc(1000000);
    CHECK(buf != NULL);
    if (!buf)
        return;
    memset(buf, 'a', 1000000);
    digest_init(&d, DIGEST_MD5);
    for (done = 0, step = 1; done < 1000000; done += step, step = step * 7 % 997 + 1) {
        if (step > 1000000 - done)
            step = 1000000 - done;
        digest_update(&d, buf + done, step);
    }
    digest_final(&d, sum);
    hex(sum, got);
    CHECK(strcmp(got, "7707d6ae4e027c70eea2a935c2296f21") == 0);
    free(buf);

    CHECK_EQ(digest_by_name("md5", 3), DIGEST_MD5);
    CHECK_EQ(digest_by_name("md4", 3), DIGEST_MD4);
    CHECK_EQ(digest_by_name("md5x", 4), 0);
    CHECK_EQ(digest_by_name("sha1", 4), 0);
}

static void test_base64(void)
{
    char out[64];

    base64_nopad((const uint8_t *)"", 0, out);
    CHECK(strcmp(out, "") == 0);
    base64_nopad((const uint8_t *)"f", 1, out);
    CHECK(strcmp(out, "Zg") == 0);
    base64_nopad((const uint8_t *)"fo", 2, out);
    CHECK(strcmp(out, "Zm8") == 0);
    base64_nopad((const uint8_t *)"foo", 3, out);
    CHECK(strcmp(out, "Zm9v") == 0);
    base64_nopad((const uint8_t *)"foobar", 6, out);
    CHECK(strcmp(out, "Zm9vYmFy") == 0);
    base64_nopad((const uint8_t *)"\xff\xfe\xfd", 3, out);
    CHECK(strcmp(out, "//79") == 0);
}

static void test_auth(void)
{
    char out[64];

    /* First of our list that the server also offers. */
    CHECK_EQ(choose_digest("md5 md4", "sha512 sha256 sha1 md5 md4"), DIGEST_MD5);
    CHECK_EQ(choose_digest("md5 md4", "md4"), DIGEST_MD4);
    CHECK_EQ(choose_digest("md4 md5", "md5 md4"), DIGEST_MD4);
    CHECK_EQ(choose_digest("md5 md4", "xxh128 xxh3 sha1"), 0);
    CHECK_EQ(choose_digest("md5 md4", "md55 xmd5"), 0);
    CHECK_EQ(choose_digest("md5", ""), 0);

    /* base64(md5("secret" + challenge)), checked with Python's hashlib. */
    auth_response(DIGEST_MD5, "secret", "aGVsbG8gY2hhbGxlbmdl", out);
    CHECK(strcmp(out, "S2MY7BsZ59j+k4Dnq6OZdQ") == 0);
    CHECK_EQ(strlen(out), 22);
}

static void test_password_file(void)
{
    static const char content[] = "\r\nhunter2\r\nsecond line\n";
    char buf[64];
    plat_file *f;

    CHECK_EQ(plat_open_write("arsync-test.pw", &f), PLAT_OK);
    CHECK_EQ(plat_write(f, content, sizeof content - 1), PLAT_OK);
    CHECK_EQ(plat_close(f), PLAT_OK);
    CHECK_EQ(read_password_file("arsync-test.pw", buf, sizeof buf), 0);
    CHECK(strcmp(buf, "hunter2") == 0);

    CHECK_EQ(plat_open_write("arsync-test.pw", &f), PLAT_OK);
    CHECK_EQ(plat_write(f, "\n\n", 2), PLAT_OK);
    CHECK_EQ(plat_close(f), PLAT_OK);
    CHECK_EQ(read_password_file("arsync-test.pw", buf, sizeof buf), -1);

    CHECK_EQ(plat_remove("arsync-test.pw"), PLAT_OK);
    CHECK_EQ(read_password_file("arsync-test.pw", buf, sizeof buf), -1);
}

static void test_server_args(void)
{
    const char *args[SESSION_MAX_ARGS];
    char buf[256], err[64];
    ars_options o;
    ars_remote r;
    int n;

    memset(&o, 0, sizeof o);
    parse_remote("rsync://h/pub", &r, err, sizeof err);
    n = build_server_args(&o, &r, 0, args, buf, sizeof buf);
    CHECK_EQ(n, 5);
    CHECK(strcmp(args[0], "--server") == 0);
    CHECK(strcmp(args[1], "--sender") == 0);
    CHECK(strcmp(args[2], "-e.fxCIvu") == 0);
    CHECK(strcmp(args[3], ".") == 0);
    CHECK(strcmp(args[4], "pub/") == 0);   /* bare module gets a slash */

    o.verbose = 2;
    parse_remote("h::pub/dir/file", &r, err, sizeof err);
    n = build_server_args(&o, &r, 0, args, buf, sizeof buf);
    CHECK_EQ(n, 5);
    CHECK(strcmp(args[2], "-vve.fxCIvu") == 0);
    CHECK(strcmp(args[4], "pub/dir/file") == 0);

    /* Pushing: the server receives, so no --sender. */
    o.verbose = 0;
    o.recursive = o.times = 1;
    parse_remote("h::up", &r, err, sizeof err);
    n = build_server_args(&o, &r, 1, args, buf, sizeof buf);
    CHECK_EQ(n, 4);
    CHECK(strcmp(args[0], "--server") == 0);
    CHECK(strcmp(args[1], "-tre.fxCIvu") == 0);
    CHECK(strcmp(args[2], ".") == 0);
    CHECK(strcmp(args[3], "up/") == 0);
}

void test_auth_suite(void)
{
    test_digests();
    test_base64();
    test_auth();
    test_password_file();
    test_server_args();
}
