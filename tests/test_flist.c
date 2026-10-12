/* test_flist.c - file-list decoding, ordering, duplicates; listing format */
#include <stdlib.h>

#include "test.h"
#include "amitime.h"
#include "flist.h"
#include "output.h"

#define DIR_MODE (WIRE_S_IFDIR | 0755)
#define REG_MODE (WIRE_S_IFREG | 0644)

/* The order rsync itself lists this tree in ("rsync -r --list-only ./"). */
static void test_order(void)
{
    static const char *const want[] = {
        ".", ".hidden", "B", "_u", "a", "a.e", "b y", "b+", "b.txt", "foo-bar", "foo.bar",
        "z", "~tilde", ".git", "b-y", "b-y/y", "b.d", "b.d/in", "b", "b/x", "d1", "d1/d2",
        "d1/d2/d3", "d1/d2/d3/deep", "foo", "foo/in",
    };
    static const char *const dirs[] = { ".", ".git", "b-y", "b.d", "b", "d1", "d1/d2",
                                        "d1/d2/d3", "foo" };
    /* Deliberately scrambled input order. */
    static const char *const input[] = {
        "foo/in", "z", "d1/d2/d3/deep", "b", ".git", "b+", "a", "b/x", "foo", ".", "b.d/in",
        "~tilde", "d1/d2", "B", "b y", "b-y", "_u", "foo-bar", "b-y/y", "a.e", "d1", "b.txt",
        ".hidden", "foo.bar", "d1/d2/d3", "b.d",
    };
    ars_flist fl;
    size_t i, j;
    int is_dir;

    flist_init(&fl);
    for (i = 0; i < sizeof input / sizeof input[0]; i++) {
        is_dir = 0;
        for (j = 0; j < sizeof dirs / sizeof dirs[0]; j++)
            if (strcmp(input[i], dirs[j]) == 0)
                is_dir = 1;
        CHECK_EQ(flist_add(&fl, input[i], is_dir ? DIR_MODE : REG_MODE, 1, 0, 0), WIRE_OK);
    }
    flist_sort_and_clean(&fl);
    CHECK_EQ(fl.count, sizeof want / sizeof want[0]);
    for (i = 0; i < fl.count && i < sizeof want / sizeof want[0]; i++) {
        if (strcmp(flist_name(&fl, &fl.files[i]), want[i]) != 0) {
            printf("  order[%lu] = %s, want %s\n", (unsigned long)i,
                   flist_name(&fl, &fl.files[i]), want[i]);
            CHECK(0);
        }
    }
    flist_free(&fl);
}

static void test_duplicates(void)
{
    ars_flist fl;

    /* Same-named file twice: the first is kept. A directory and a file of
     * the same name: the directory is kept. Two such directories: the
     * first is kept. Removed entries keep their slots. */
    flist_init(&fl);
    flist_add(&fl, "x", REG_MODE, 1, 100, 0);
    flist_add(&fl, "x", REG_MODE, 2, 200, 0);
    flist_add(&fl, "d", REG_MODE, 3, 0, 0);
    flist_add(&fl, "d", DIR_MODE, 4, 0, 0);
    flist_add(&fl, "e", DIR_MODE, 5, 0, 0);
    flist_add(&fl, "e", DIR_MODE, 6, 0, 0);
    flist_add(&fl, "d/in", REG_MODE, 7, 0, 0);
    flist_sort_and_clean(&fl);

    /* Sorted: d(file) x x | d(dir) d/in e e */
    CHECK_EQ(fl.count, 7);
    CHECK_EQ(fl.files[0].mode, 0);               /* file "d" lost to dir "d" */
    CHECK_EQ(fl.files[1].size, 1);               /* first "x" kept... */
    CHECK_EQ(fl.files[2].mode, 0);               /* ...second dropped */
    CHECK(strcmp(flist_name(&fl, &fl.files[3]), "d") == 0);
    CHECK_EQ(fl.files[3].size, 4);
    CHECK(strcmp(flist_name(&fl, &fl.files[4]), "d/in") == 0);
    CHECK_EQ(fl.files[5].size, 5);               /* first dir "e" kept */
    CHECK_EQ(fl.files[6].mode, 0);
    flist_free(&fl);
}

/* Builds a file list on the wire the way a sender does. */
static void put_entry(ars_writer *w, int varint, uint32_t xflags, int l1, const char *suffix,
                      int64_t size, int64_t mtime, uint32_t nsec, uint32_t mode)
{
    size_t l2 = strlen(suffix);

    if (varint) {
        w_varint(w, (int32_t)xflags);
    } else {
        w_byte(w, (uint8_t)xflags);
        if (xflags & XMIT_EXTENDED_FLAGS)
            w_byte(w, (uint8_t)(xflags >> 8));
    }
    if (xflags & XMIT_SAME_NAME)
        w_byte(w, (uint8_t)l1);
    if (xflags & XMIT_LONG_NAME)
        w_varint(w, (int32_t)l2);
    else
        w_byte(w, (uint8_t)l2);
    w_bytes(w, suffix, l2);
    w_varlong(w, size, 3);
    if (!(xflags & XMIT_SAME_TIME))
        w_varlong(w, mtime, 4);
    if (xflags & XMIT_MOD_NSEC)
        w_varint(w, (int32_t)nsec);
    if (!(xflags & XMIT_SAME_MODE))
        w_int32(w, (int32_t)mode);
}

static void test_recv(int varint)
{
    char longname[300], err[128];
    ars_flist_format fmt;
    ars_membuf mb;
    ars_reader r;
    ars_writer w;
    ars_flist fl;

    memset(longname, 'n', sizeof longname - 1);
    longname[sizeof longname - 1] = '\0';

    membuf_init(&mb);
    membuf_writer(&w, &mb);
    put_entry(&w, varint, XMIT_TOP_DIR | (varint ? 0 : XMIT_EXTENDED_FLAGS),
              0, ".", 4096, 1700000000, 0, DIR_MODE);
    put_entry(&w, varint, XMIT_MOD_NSEC | XMIT_EXTENDED_FLAGS, 0, "file-one", 5000000000LL,
              1700000001, 123456789, REG_MODE);
    /* Shares "file-" with the previous name, same time and mode. */
    put_entry(&w, varint, XMIT_SAME_NAME | XMIT_SAME_TIME | XMIT_SAME_MODE, 5, "two",
              0, 0, 0, 0);
    put_entry(&w, varint, XMIT_LONG_NAME | XMIT_SAME_MODE, 0, longname, 7, -1000000, 0, 0);
    if (varint) {
        w_varint(&w, 0);
        w_varint(&w, IOERR_GENERAL);
    } else {
        w_byte(&w, XMIT_EXTENDED_FLAGS);
        w_byte(&w, XMIT_IO_ERROR_ENDLIST >> 8);
        w_varint(&w, IOERR_VANISHED);
    }
    w_byte(&w, 0x42);   /* following data must be left unread */

    fmt.varint_flags = varint;
    fmt.safe_flist = 1;
    flist_init(&fl);
    membuf_reader(&r, &mb);
    CHECK_EQ(flist_recv(&fl, &r, &fmt, err, sizeof err), WIRE_OK);
    CHECK_EQ(fl.count, 4);
    CHECK_EQ(fl.io_error, varint ? IOERR_GENERAL : IOERR_VANISHED);
    CHECK_EQ(r_byte(&r), 0x42);
    if (fl.count == 4) {
        /* Sorted: ".", "file-one", "file-two", "nnn..." */
        CHECK(strcmp(flist_name(&fl, &fl.files[0]), ".") == 0);
        CHECK(WIRE_ISDIR(fl.files[0].mode));
        CHECK(strcmp(flist_name(&fl, &fl.files[1]), "file-one") == 0);
        CHECK_EQ(fl.files[1].size, 5000000000LL);
        CHECK_EQ(fl.files[1].mtime, 1700000001);
        CHECK_EQ(fl.files[1].mtime_nsec, 123456789);
        CHECK(strcmp(flist_name(&fl, &fl.files[2]), "file-two") == 0);
        CHECK_EQ(fl.files[2].mtime, 1700000001);
        CHECK_EQ(fl.files[2].mode, REG_MODE);
        CHECK_EQ(fl.files[2].mtime_nsec, 0);
        CHECK(strcmp(flist_name(&fl, &fl.files[3]), longname) == 0);
        CHECK_EQ(fl.files[3].mtime, -1000000);
        CHECK_EQ(fl.files[3].mode, REG_MODE);
    }
    flist_free(&fl);
    membuf_free(&mb);
}

static int recv_one(const char *name, char *err, size_t errlen)
{
    ars_flist_format fmt = { 1, 1 };
    ars_membuf mb;
    ars_reader r;
    ars_writer w;
    ars_flist fl;
    int rc;

    membuf_init(&mb);
    membuf_writer(&w, &mb);
    put_entry(&w, 1, XMIT_LONG_NAME, 0, name, 1, 0, 0, REG_MODE);   /* flags 0 would end the list */
    w_varint(&w, 0);
    w_varint(&w, 0);
    flist_init(&fl);
    membuf_reader(&r, &mb);
    rc = flist_recv(&fl, &r, &fmt, err, errlen);
    flist_free(&fl);
    membuf_free(&mb);
    return rc;
}

static void test_unsafe_names(void)
{
    static const char *const bad[] = { "../x", "/etc/passwd", "a/../b", "a//b", "a/./b", "..", "a/" };
    static const char *const good[] = { "a", "a/b", "...", "a/..b", "b../c", ".x" };
    char err[128];
    size_t i;

    for (i = 0; i < sizeof bad / sizeof bad[0]; i++)
        CHECK_EQ(recv_one(bad[i], err, sizeof err), WIRE_EPROTO);
    for (i = 0; i < sizeof good / sizeof good[0]; i++)
        CHECK_EQ(recv_one(good[i], err, sizeof err), WIRE_OK);
}

static void test_truncated(void)
{
    ars_flist_format fmt = { 1, 1 };
    char err[64];
    ars_membuf mb;
    ars_reader r;
    ars_writer w;
    ars_flist fl;

    membuf_init(&mb);
    membuf_writer(&w, &mb);
    put_entry(&w, 1, XMIT_LONG_NAME, 0, "abc", 1, 0, 0, REG_MODE);
    mb.len -= 2;    /* cut into the mode */
    flist_init(&fl);
    membuf_reader(&r, &mb);
    CHECK_EQ(flist_recv(&fl, &r, &fmt, err, sizeof err), WIRE_EOF);
    flist_free(&fl);
    membuf_free(&mb);
}

static void test_civil(void)
{
    static const struct { int64_t t; int y, mo, d, h, mi, s; } v[] = {
        { 0, 1970, 1, 1, 0, 0, 0 },
        { -1, 1969, 12, 31, 23, 59, 59 },
        { 951782400, 2000, 2, 29, 0, 0, 0 },
        { 252460800, 1978, 1, 1, 0, 0, 0 },
        { 4107542399LL, 2100, 2, 28, 23, 59, 59 },
        { -2208988800LL, 1900, 1, 1, 0, 0, 0 },
        { 1704153599, 2024, 1, 1, 23, 59, 59 },
    };
    ars_tm tm;
    size_t i;

    for (i = 0; i < sizeof v / sizeof v[0]; i++) {
        secs_to_civil(v[i].t, &tm);
        CHECK(tm.year == v[i].y && tm.month == v[i].mo && tm.day == v[i].d
              && tm.hour == v[i].h && tm.minute == v[i].mi && tm.second == v[i].s);
    }
}

static void test_formatting(void)
{
    char buf[48];

    CHECK(strcmp(comma_num(0, buf), "0") == 0);
    CHECK(strcmp(comma_num(999, buf), "999") == 0);
    CHECK(strcmp(comma_num(1000, buf), "1,000") == 0);
    CHECK(strcmp(comma_num(1234567, buf), "1,234,567") == 0);
    CHECK(strcmp(comma_num(5000000000LL, buf), "5,000,000,000") == 0);
    CHECK(strcmp(comma_num(-1234, buf), "-1,234") == 0);
    CHECK(strcmp(comma_num_frac2(19000, buf), "190.00") == 0);
    CHECK(strcmp(comma_num_frac2(625936994, buf), "6,259,369.94") == 0);
    CHECK(strcmp(comma_num_frac2(7, buf), "0.07") == 0);

    CHECK(strcmp(permstring(0100644, buf), "-rw-r--r--") == 0);
    CHECK(strcmp(permstring(040755, buf), "drwxr-xr-x") == 0);
    CHECK(strcmp(permstring(0104755, buf), "-rwsr-xr-x") == 0);
    CHECK(strcmp(permstring(0102750, buf), "-rwxr-s---") == 0);
    CHECK(strcmp(permstring(041777, buf), "drwxrwxrwt") == 0);
    CHECK(strcmp(permstring(0101644, buf), "-rw-r--r-T") == 0);
    CHECK(strcmp(permstring(0104644, buf), "-rwSr--r--") == 0);
    CHECK(strcmp(permstring(0120777, buf), "lrwxrwxrwx") == 0);
    CHECK(strcmp(permstring(0010600, buf), "prw-------") == 0);
}

void test_flist_suite(void)
{
    test_order();
    test_duplicates();
    test_recv(1);
    test_recv(0);
    test_unsafe_names();
    test_truncated();
    test_civil();
    test_formatting();
}
