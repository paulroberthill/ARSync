/* test_fsmap.c - AmigaDOS name rules and progress formatting */
#include "test.h"
#include "flist.h"
#include "fsmap.h"
#include "progress.h"

void test_fsmap_suite(void)
{
    ars_fsrules r = { 30, 1, 0x7FFFFFFF };
    uint8_t flags[8];
    ars_flist fl;
    char buf[24];

    CHECK(fsmap_check_name(&r, "normal") == NULL);
    CHECK(fsmap_check_name(&r, ".") == NULL);
    CHECK(fsmap_check_name(&r, "dir/abcdefghijklmnopqrstuvwxyz0123") == NULL);      /* 30 */
    CHECK(fsmap_check_name(&r, "dir/abcdefghijklmnopqrstuvwxyz01234") != NULL);     /* 31 */
    CHECK(fsmap_check_name(&r, "a:b") != NULL);
    CHECK(fsmap_check_name(&r, "dir/a:b") != NULL);
    /* Only the last component counts; the parent was checked on its own. */
    CHECK(fsmap_check_name(&r, "a-very-long-directory-name-over-thirty/x") == NULL);
    r.name_max = 0;
    CHECK(fsmap_check_name(&r, "dir/abcdefghijklmnopqrstuvwxyz01234") == NULL);

    flist_init(&fl);
    flist_add(&fl, "README", 0100644, 1, 0, 0);     /* 0 */
    flist_add(&fl, "Readme", 0100644, 1, 0, 0);     /* 1: clashes with 0 */
    flist_add(&fl, "readme", 0100644, 1, 0, 0);     /* 2: clashes with 0 */
    flist_add(&fl, "other", 0100644, 1, 0, 0);      /* 3 */
    flist_add(&fl, "Dir/x", 0100644, 1, 0, 0);      /* 4 */
    flist_add(&fl, "dir/y", 0100644, 1, 0, 0);      /* 5: different file, no clash */
    flist_add(&fl, "gone", 0, 1, 0, 0);             /* 6: removed duplicate */
    flist_add(&fl, "GONE", 0100644, 1, 0, 0);       /* 7: nothing to clash with */
    memset(flags, 0, sizeof flags);
    CHECK_EQ(fsmap_find_case_clashes(&fl, flags, 4), 2);
    CHECK_EQ(flags[0], 0);
    CHECK_EQ(flags[1], 4);
    CHECK_EQ(flags[2], 4);
    CHECK_EQ(flags[3] | flags[4] | flags[5] | flags[6] | flags[7], 0);
    flist_free(&fl);

    /* kB = 1024 bytes; values as rsync prints them. */
    CHECK(strcmp(progress_rate(2, 1000, buf), "   0.00kB/s") == 0);
    CHECK(strcmp(progress_rate(2048, 1000, buf), "   2.00kB/s") == 0);
    CHECK(strcmp(progress_rate(1536, 500, buf), "   3.00kB/s") == 0);
    CHECK(strcmp(progress_rate(10 * 1048576, 1000, buf), "  10.00MB/s") == 0);
    CHECK(strcmp(progress_rate((int64_t)3 * 1073741824, 1000, buf), "   3.00GB/s") == 0);
    CHECK(strcmp(progress_rate(100, 0, buf), "  97.65kB/s") == 0);
}
