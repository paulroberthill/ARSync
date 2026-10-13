/* test_filter.c - wildcard matching, filter rules, and the M8 options */
#include <stdlib.h>

#include "test.h"
#include "filter.h"
#include "options.h"
#include "plat.h"
#include "session.h"
#include "url.h"

static void test_wildmatch(void)
{
    static const struct { const char *pat, *text; int want; } v[] = {
        { "foo", "foo", 1 },          { "foo", "fo", 0 },           { "foo", "foox", 0 },
        { "*", "foo", 1 },            { "*", "", 1 },               { "*", "foo/bar", 0 },
        { "**", "foo/bar", 1 },       { "f*", "foo/bar", 0 },       { "f**", "foo/bar", 1 },
        { "f?o", "foo", 1 },          { "f?o", "f/o", 0 },          { "???", "ab", 0 },
        { "*.c", "main.c", 1 },       { "*.c", "main.o", 0 },       { "*.c", "src/main.c", 0 },
        { "a*b*c", "axxbyyc", 1 },    { "a*b*c", "axxbyy", 0 },     { "*x*", "abc", 0 },
        { "[ab]*", "apple", 1 },      { "[ab]*", "cherry", 0 },     { "[!ab]*", "apple", 0 },
        { "[^ab]*", "cherry", 1 },    { "[a-c]x", "bx", 1 },        { "[a-c]x", "dx", 0 },
        { "[]]", "]", 1 },            { "[a-]", "-", 1 },           { "[!]]", "a", 1 },
        { "[[:digit:]]*", "1abc", 1 }, { "[[:digit:]]*", "abc", 0 }, { "[[:upper:]]", "Q", 1 },
        { "[/]", "/", 0 },            { "\\*", "*", 1 },            { "\\*", "a", 0 },
        { "a\\?", "a?", 1 },          { "a\\?", "ab", 0 },
        { "d1/**/deep", "d1/d2/d3/deep", 1 },  { "d1/**/deep", "d1/deep", 0 },
        { "d1/*/deep", "d1/d2/deep", 1 },      { "d1/*/deep", "d1/d2/d3/deep", 0 },
        { "**/tmp", "build/tmp", 1 },          { "**/tmp", "a/b/tmp", 1 },
        { "*/x", "a/b/x", 0 },
    };
    size_t i;
    int got;

    for (i = 0; i < sizeof v / sizeof v[0]; i++) {
        got = wildmatch(v[i].pat, v[i].text);
        if (got != v[i].want)
            printf("  wildmatch(\"%s\", \"%s\") = %d, want %d\n", v[i].pat, v[i].text, got, v[i].want);
        CHECK_EQ(got, v[i].want);
    }
}

/* Each case: rules (as --filter text, NULL-terminated), then names with
 * "/" appended for directories, and whether each is excluded. */
static void rules_case(const char *const *rules, const char *const *names, const int *want)
{
    char err[128], name[256];
    ars_filters f;
    size_t i, len;
    int is_dir, got;

    filters_init(&f);
    for (i = 0; rules[i]; i++)
        CHECK_EQ(filters_add(&f, rules[i], FILTER_ADD_RULE, err, sizeof err), 0);
    for (i = 0; names[i]; i++) {
        snprintf(name, sizeof name, "%s", names[i]);
        len = strlen(name);
        is_dir = len > 1 && name[len - 1] == '/';
        if (is_dir)
            name[len - 1] = '\0';
        got = filters_excluded(&f, name, is_dir);
        if (got != want[i])
            printf("  rules starting \"%s\": \"%s\" excluded = %d, want %d\n", rules[0], names[i], got, want[i]);
        CHECK_EQ(got, want[i]);
    }
    filters_free(&f);
}

static void test_rules(void)
{
    static const char *const r1[] = { "- *.o", NULL };
    static const char *const n1[] = { "main.o", "src/main.o", "src/main.c", "x.o/", NULL };
    static const int w1[] = { 1, 1, 0, 1 };

    /* Trailing slash: directories only. */
    static const char *const r2[] = { "- src/", NULL };
    static const char *const n2[] = { "src/", "src", "x/src/", "srcx/", NULL };
    static const int w2[] = { 1, 0, 1, 0 };

    /* Leading slash: anchored at the top of the transfer. */
    static const char *const r3[] = { "- /b", "- /d1/d2", NULL };
    static const char *const n3[] = { "b", "b/", "x/b", "x/b/", "d1/d2/", "x/d1/d2/", NULL };
    static const int w3[] = { 1, 1, 0, 0, 1, 0 };

    /* An inner slash: matched against that many trailing components. */
    static const char *const r4[] = { "- d1/d2", "- a/*/c", NULL };
    static const char *const n4[] = { "d1/d2", "x/d1/d2", "xd1/d2", "d1/d2/d3", "q/a/b/c", "a/b/b/c", NULL };
    static const int w4[] = { 1, 1, 0, 0, 1, 0 };

    /* "**" can span directories, and may match after any slash; a leading
     * "**" + "/" also matches no directories at all. */
    static const char *const r5[] = { "- d1/**/deep", "- **/tmp", NULL };
    static const char *const n5[] = { "d1/d2/d3/deep", "x/d1/d2/deep", "d1/deep", "tmp/", "a/b/tmp/",
                                      "atmp", NULL };
    static const int w5[] = { 1, 1, 0, 1, 1, 0 };

    /* First match wins; "dir/" + "***" is the directory and its contents. */
    static const char *const r6[] = { "+ docs/***", "- *.txt", NULL };
    static const char *const n6[] = { "docs/", "docs/a.txt", "docs/sub/b.txt", "x.txt", "docs", NULL };
    static const int w6[] = { 0, 0, 0, 1, 0 };

    /* The classic "only *.c files" set. */
    static const char *const r7[] = { "+ */", "+ *.c", "- *", NULL };
    static const char *const n7[] = { "src/", "src/main.c", "src/main.o", "README", NULL };
    static const int w7[] = { 0, 0, 1, 1 };

    /* "!" clears what came before; rule words work too. */
    static const char *const r8[] = { "- *.o", "!", "exclude *.c", "include x*", NULL };
    static const char *const n8[] = { "a.o", "a.c", "x.c", NULL };
    static const int w8[] = { 0, 1, 1 };

    /* No rules: nothing excluded; and the top of the transfer never is. */
    static const char *const r9[] = { "- *", NULL };
    static const char *const n9[] = { "", NULL };
    static const int w9[] = { 0 };

    rules_case(r1, n1, w1);
    rules_case(r2, n2, w2);
    rules_case(r3, n3, w3);
    rules_case(r4, n4, w4);
    rules_case(r5, n5, w5);
    rules_case(r6, n6, w6);
    rules_case(r7, n7, w7);
    rules_case(r8, n8, w8);
    rules_case(r9, n9, w9);
}

static void test_rule_parsing(void)
{
    static const uint8_t wire[] = {
        5, 0, 0, 0, '-', ' ', '*', '.', 'o',
        6, 0, 0, 0, '+', ' ', 'd', 'i', 'r', '/',
        6, 0, 0, 0, '-', ' ', '/', 'a', ' ', 'b',
        0, 0, 0, 0,
    };
    static const char content[] = "# comment\r\n; also\n*.o\r\n\n+ dir/\n- /a b";
    char err[128];
    ars_filters f;
    ars_membuf mb;
    ars_writer w;
    plat_file *fh;

    filters_init(&f);
    CHECK_EQ(filters_add(&f, "P foo", FILTER_ADD_RULE, err, sizeof err), -1);
    CHECK_EQ(filters_add(&f, "-foo", FILTER_ADD_RULE, err, sizeof err), -1);
    CHECK_EQ(filters_add(&f, "", FILTER_ADD_EXCLUDE, err, sizeof err), -1);
    CHECK_EQ(f.count, 0);

    /* A file: comments and blank lines skipped, CRLF and a missing final
     * newline handled, "+ "/"- " prefixes honoured. */
    CHECK_EQ(plat_open_write("arsync-test.filter", &fh), PLAT_OK);
    CHECK_EQ(plat_write(fh, content, sizeof content - 1), PLAT_OK);
    CHECK_EQ(plat_close(fh), PLAT_OK);
    CHECK_EQ(filters_add_file(&f, "arsync-test.filter", FILTER_ADD_EXCLUDE, err, sizeof err), 0);
    CHECK_EQ(f.count, 3);
    CHECK(f.count == 3 && strcmp(f.rules[0].pattern, "*.o") == 0 && !(f.rules[0].flags & FILTER_INCLUDE));
    CHECK(f.count == 3 && strcmp(f.rules[1].pattern, "dir") == 0
          && (f.rules[1].flags & (FILTER_INCLUDE | FILTER_DIR_ONLY)) == (FILTER_INCLUDE | FILTER_DIR_ONLY));
    CHECK(f.count == 3 && strcmp(f.rules[2].pattern, "/a b") == 0);
    CHECK_EQ(plat_remove("arsync-test.filter"), PLAT_OK);
    CHECK_EQ(filters_add_file(&f, "arsync-test.filter", FILTER_ADD_EXCLUDE, err, sizeof err), -1);

    /* On the wire: int32 length, then the rule with its prefix. */
    membuf_init(&mb);
    membuf_writer(&w, &mb);
    filters_send(&f, &w);
    CHECK_EQ(mb.len, sizeof wire);
    CHECK(mb.len == sizeof wire && memcmp(mb.data, wire, sizeof wire) == 0);
    membuf_free(&mb);
    filters_free(&f);
}

static void test_m8_options(void)
{
    char *argv1[] = { "arsync", "-rz", "--delete", "--exclude=*.o", "--include", "x/",
                      "--filter=- /y", "--compress-level=9", "src", "dst" };
    char *argv2[] = { "arsync", "--delete", "a", "b" };
    char *argv3[] = { "arsync", "--compress-level=10", "a" };
    char *argv4[] = { "arsync", "--zc=zstd", "a" };
    char *argv5[] = { "arsync", "-r", "--delete-excluded", "--zc", "zlib", "a" };
    char *argv6[] = { "arsync", "--filter=P x", "a" };
    char *argv7[] = { "arsync", "--exclude-from=no-such-file", "a" };
    const char *args[SESSION_MAX_ARGS];
    char buf[256], err[128];
    ars_options o;
    ars_remote r;
    int n;

    CHECK_EQ(options_parse(&o, 10, argv1, err, sizeof err), 0);
    CHECK(o.recursive && o.compress && o.delete_mode && !o.delete_excluded);
    CHECK_EQ(o.compress_level, 9);
    CHECK_EQ(o.compress_choice, 0);
    CHECK_EQ(o.filters.count, 3);
    CHECK(o.filters.count == 3 && (o.filters.rules[1].flags & FILTER_INCLUDE));

    /* Pulling: we delete, so the server isn't told; -z and the level are. */
    parse_remote("h::m/p", &r, err, sizeof err);
    n = build_server_args(&o, &r, 0, args, buf, sizeof buf);
    CHECK_EQ(n, 6);
    CHECK(strcmp(args[2], "-rze.fxCIvu") == 0);
    CHECK(strcmp(args[3], "--compress-level=9") == 0);
    /* Pushing: the server deletes. */
    n = build_server_args(&o, &r, 1, args, buf, sizeof buf);
    CHECK_EQ(n, 6);
    CHECK(strcmp(args[1], "-rze.fxCIvu") == 0);
    CHECK(strcmp(args[2], "--delete") == 0);
    options_free(&o);

    CHECK_EQ(options_parse(&o, 4, argv2, err, sizeof err), -1);   /* --delete needs -r */
    options_free(&o);
    CHECK_EQ(options_parse(&o, 3, argv3, err, sizeof err), -1);
    options_free(&o);
    CHECK_EQ(options_parse(&o, 3, argv4, err, sizeof err), -1);
    options_free(&o);
    CHECK_EQ(options_parse(&o, 6, argv5, err, sizeof err), 0);
    CHECK(o.delete_mode && o.delete_excluded && o.compress);
    CHECK_EQ(o.compress_choice, CPRES_ZLIB);
    n = build_server_args(&o, &r, 1, args, buf, sizeof buf);
    CHECK_EQ(n, 6);
    CHECK(strcmp(args[1], "-rze.fxCIvu") == 0);
    CHECK(strcmp(args[2], "--delete-excluded") == 0);
    CHECK(strcmp(args[3], "--old-compress") == 0);
    options_free(&o);
    CHECK_EQ(options_parse(&o, 3, argv6, err, sizeof err), -1);
    options_free(&o);
    CHECK_EQ(options_parse(&o, 3, argv7, err, sizeof err), -1);
    options_free(&o);
}

void test_filter_suite(void)
{
    test_wildmatch();
    test_rules();
    test_rule_parsing();
    test_m8_options();
}
