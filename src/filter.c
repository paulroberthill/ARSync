/* filter.c - include/exclude rules
 *
 * Rules are checked in order and the first match decides. How a pattern
 * is matched depends on its shape, as in rsync:
 *   - a trailing '/' makes it match directories only;
 *   - a leading '/' anchors it at the top of the transfer;
 *   - without a '/' (or "**") it's matched against the last component;
 *   - with one, against as many trailing components as it has (or, with
 *     "**", against the whole path or any tail of it);
 *   - "dir/" followed by "***" matches dir itself and everything below it.
 * The client sends the same rules to the server, which applies them to
 * its side of the transfer.
 */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "filter.h"
#include "plat.h"

#define FILTER_LINE_MAX 1024

#define WM_TRUE 1
#define WM_FALSE 0
#define WM_ABORT_ALL (-1)
#define WM_ABORT_TO_STARSTAR (-2)

static int class_match(const char *name, size_t len, unsigned char c)
{
    static const struct { const char *name; int (*fn)(int); } classes[] = {
        { "alnum", isalnum }, { "alpha", isalpha }, { "blank", isblank },
        { "cntrl", iscntrl }, { "digit", isdigit }, { "graph", isgraph },
        { "lower", islower }, { "print", isprint }, { "punct", ispunct },
        { "space", isspace }, { "upper", isupper }, { "xdigit", isxdigit },
    };
    size_t i;

    for (i = 0; i < sizeof classes / sizeof classes[0]; i++)
        if (strlen(classes[i].name) == len && memcmp(classes[i].name, name, len) == 0)
            return classes[i].fn(c) != 0;
    return -1;
}

/* Matches a "[...]" class at *pp against c, leaving *pp on the ']'.
 * Returns 1/0, or -1 for a malformed class. */
static int bracket(const char **pp, unsigned char c)
{
    const char *p = *pp + 1;
    int negated = 0, matched = 0, r;
    unsigned char prev = 0, lo, hi;
    const char *end;

    if (*p == '!' || *p == '^') {
        negated = 1;
        p++;
    }
    for (;; p++) {
        if (*p == '\0')
            return -1;
        if (*p == ']' && p > *pp + 1 + negated)
            break;
        if (*p == '\\') {
            if (!*++p)
                return -1;
            if ((unsigned char)*p == c)
                matched = 1;
            prev = (unsigned char)*p;
        } else if (*p == '-' && prev && p[1] && p[1] != ']') {
            lo = prev;
            hi = (unsigned char)*++p;
            if (hi == '\\' && !(hi = (unsigned char)*++p))
                return -1;
            if (c >= lo && c <= hi)
                matched = 1;
            prev = 0;
        } else if (*p == '[' && p[1] == ':' && (end = strstr(p + 2, ":]")) != NULL) {
            if ((r = class_match(p + 2, (size_t)(end - p - 2), c)) < 0)
                return -1;
            if (r)
                matched = 1;
            p = end + 1;
            prev = 0;
        } else {
            if ((unsigned char)*p == c)
                matched = 1;
            prev = (unsigned char)*p;
        }
    }
    *pp = p;
    return matched != negated;
}

static int dowild(const char *p, const char *t)
{
    int matched, special;

    for (; *p; t++, p++) {
        if (*t == '\0' && *p != '*')
            return WM_ABORT_ALL;
        switch (*p) {
        case '\\':
            if (*++p != *t)
                return WM_FALSE;
            break;
        case '?':
            if (*t == '/')
                return WM_FALSE;
            break;
        case '*':
            if (*++p == '*') {
                while (*++p == '*')
                    ;
                special = 1;
            } else {
                special = 0;
            }
            if (*p == '\0') {
                /* A trailing "**" matches anything; '*' only what has no
                 * more slashes. */
                return special || !strchr(t, '/') ? WM_TRUE : WM_FALSE;
            }
            for (; *t; t++) {
                if ((matched = dowild(p, t)) != WM_FALSE) {
                    if (!special || matched != WM_ABORT_TO_STARSTAR)
                        return matched;
                } else if (!special && *t == '/') {
                    return WM_ABORT_TO_STARSTAR;
                }
            }
            return WM_ABORT_ALL;
        case '[':
            if (*t == '/' || (matched = bracket(&p, (unsigned char)*t)) < 0)
                return WM_FALSE;
            if (!matched)
                return WM_FALSE;
            break;
        default:
            if (*p != *t)
                return WM_FALSE;
        }
    }
    return *t == '\0' ? WM_TRUE : WM_FALSE;
}

int wildmatch(const char *pattern, const char *text)
{
    return dowild(pattern, text) == WM_TRUE;
}

/* Like wildmatch, at the start of text (where == 0), after its last N
 * slashes (where == N > 0), or at the start and after every slash
 * (where < 0). */
static int wildmatch_where(const char *pattern, const char *text, int where)
{
    const char *t;
    int matched;

    if (where > 0) {
        for (t = text + strlen(text); t > text; t--)
            if (t[-1] == '/' && --where == 0)
                break;
        text = t;
    }
    matched = dowild(pattern, text);
    if (matched == WM_TRUE)
        return 1;
    if (where < 0 && matched != WM_ABORT_ALL) {
        for (t = text; *t; t++) {
            if (*t == '/' && (matched = dowild(pattern, t + 1)) != WM_FALSE
                && matched != WM_ABORT_TO_STARSTAR)
                return matched == WM_TRUE;
        }
    }
    return 0;
}

static int rule_matches(const ars_filter_rule *r, const char *name, int is_dir)
{
    const char *pattern = r->pattern, *slash;
    char rooted[FILTER_LINE_MAX + 1];
    int anchored = 0, where;
    size_t nlen, plen;

    if (!*name)
        return 0;
    if (!(r->flags & FILTER_WILD2) && !r->slash_cnt) {
        /* No slash in the pattern: just the last component. */
        if ((slash = strrchr(name, '/')) != NULL)
            name = slash + 1;
    }
    if (*pattern == '/') {
        anchored = 1;
        pattern++;
    }

    if (r->flags & FILTER_WILD) {
        if (!anchored && r->slash_cnt && !(r->flags & FILTER_WILD2))
            where = r->slash_cnt + 1;
        else if (!anchored && (r->flags & FILTER_WILD2) && !(r->flags & FILTER_WILD2_PREFIX))
            where = -1;
        else
            where = 0;
        if (wildmatch_where(pattern, name, where))
            return 1;
        /* A leading "**" + "/" also matches no directories at all, so
         * "**" + "/tmp" catches a top-level "tmp" too: match as if the
         * name started with a slash. */
        nlen = strlen(name);
        if ((r->flags & FILTER_WILD2_PREFIX) && nlen < sizeof rooted - 1) {
            rooted[0] = '/';
            memcpy(rooted + 1, name, nlen + 1);
            return wildmatch_where(pattern, rooted, where);
        }
        return 0;
    }
    if (anchored)
        return strcmp(name, pattern) == 0;
    /* A literal: a whole trailing component (or components). */
    nlen = strlen(name);
    plen = strlen(pattern);
    return plen <= nlen && strcmp(name + nlen - plen, pattern) == 0
           && (plen == nlen || name[nlen - plen - 1] == '/');
}

int filters_excluded(const ars_filters *f, const char *name, int is_dir)
{
    char dirpat[FILTER_LINE_MAX];
    const ars_filter_rule *r;
    size_t i, len;
    int hit;

    for (i = 0; i < f->count; i++) {
        r = &f->rules[i];
        if (r->flags & FILTER_WILD3_SUFFIX) {
            /* "dir/" + "***": anything inside, or the directory itself (as if
             * the rule were "dir/"). */
            hit = rule_matches(r, name, is_dir);
            len = strlen(r->pattern) - 4;
            if (!hit && is_dir && len < sizeof dirpat) {
                ars_filter_rule dir = *r;

                memcpy(dirpat, r->pattern, len);
                dirpat[len] = '\0';
                dir.pattern = dirpat;
                dir.flags &= ~(FILTER_WILD3_SUFFIX | FILTER_WILD2 | FILTER_WILD2_PREFIX);
                if (!strpbrk(dirpat, "*?["))
                    dir.flags &= ~FILTER_WILD;
                dir.slash_cnt--;
                hit = rule_matches(&dir, name, is_dir);
            }
        } else if ((r->flags & FILTER_DIR_ONLY) && !is_dir) {
            continue;
        } else {
            hit = rule_matches(r, name, is_dir);
        }
        if (hit)
            return !(r->flags & FILTER_INCLUDE);
    }
    return 0;
}

static void clear(ars_filters *f)
{
    size_t i;

    for (i = 0; i < f->count; i++)
        free(f->rules[i].pattern);
    f->count = 0;
}

void filters_init(ars_filters *f)
{
    memset(f, 0, sizeof *f);
}

void filters_free(ars_filters *f)
{
    clear(f);
    free(f->rules);
    memset(f, 0, sizeof *f);
}

static int add_rule(ars_filters *f, const char *pat, size_t len, unsigned flags,
                    char *err, size_t errlen)
{
    ars_filter_rule *r, *grown;
    const char *s;
    size_t cap;

    if (len > 1 && pat[len - 1] == '/') {
        flags |= FILTER_DIR_ONLY;
        len--;
    }
    if (len == 0) {
        snprintf(err, errlen, "empty filter pattern");
        return -1;
    }
    if (f->count == f->cap) {
        cap = f->cap ? f->cap * 2 : 16;
        if (!(grown = realloc(f->rules, cap * sizeof *grown)))
            goto nomem;
        f->rules = grown;
        f->cap = cap;
    }
    r = &f->rules[f->count];
    if (!(r->pattern = malloc(len + 1)))
        goto nomem;
    memcpy(r->pattern, pat, len);
    r->pattern[len] = '\0';
    if (strpbrk(r->pattern, "*?[")) {
        flags |= FILTER_WILD;
        if ((s = strstr(r->pattern, "**")) != NULL) {
            flags |= FILTER_WILD2;
            if (s == r->pattern)
                flags |= FILTER_WILD2_PREFIX;
            if (len >= 4 && strcmp(r->pattern + len - 4, "/***") == 0)
                flags |= FILTER_WILD3_SUFFIX;
        }
    }
    r->flags = flags;
    r->slash_cnt = 0;
    for (s = r->pattern; (s = strchr(s, '/')) != NULL; s++)
        r->slash_cnt++;
    f->count++;
    return 0;

nomem:
    snprintf(err, errlen, "out of memory");
    return -1;
}

int filters_add(ars_filters *f, const char *text, int how, char *err, size_t errlen)
{
    unsigned flags = how == FILTER_ADD_INCLUDE ? FILTER_INCLUDE : 0;
    const char *pat = text;

    if (how == FILTER_ADD_RULE) {
        /* "- PAT", "+ PAT" (or with '_' for the space), "exclude PAT",
         * "include PAT"; "!" or "clear" empties the list. */
        if (strcmp(text, "!") == 0 || strcmp(text, "clear") == 0) {
            clear(f);
            return 0;
        }
        if ((text[0] == '-' || text[0] == '+') && (text[1] == ' ' || text[1] == '_')) {
            flags = text[0] == '+' ? FILTER_INCLUDE : 0;
            pat = text + 2;
        } else if (strncmp(text, "exclude ", 8) == 0) {
            flags = 0;
            pat = text + 8;
        } else if (strncmp(text, "include ", 8) == 0) {
            flags = FILTER_INCLUDE;
            pat = text + 8;
        } else {
            snprintf(err, errlen, "unsupported filter rule: %s"
                     " (arsync knows \"+ PATTERN\", \"- PATTERN\" and \"!\")", text);
            return -1;
        }
    } else if (strcmp(text, "!") == 0) {
        clear(f);
        return 0;
    } else if ((text[0] == '-' || text[0] == '+') && text[1] == ' ') {
        /* rsync's --exclude/--include accept the same prefixes. */
        flags = text[0] == '+' ? FILTER_INCLUDE : 0;
        pat = text + 2;
    }
    return add_rule(f, pat, strlen(pat), flags, err, errlen);
}

int filters_add_file(ars_filters *f, const char *path, int how, char *err, size_t errlen)
{
    char line[FILTER_LINE_MAX], buf[512];
    size_t len = 0, i;
    plat_file *fh;
    long n;
    int rc, eof = 0, too_long = 0;

    if ((rc = plat_open_read(path, &fh)) != PLAT_OK) {
        snprintf(err, errlen, "failed to open filter file %s: %s", path, plat_strerror(rc));
        return -1;
    }
    while (!eof) {
        if ((n = plat_read(fh, buf, sizeof buf)) < 0) {
            plat_close(fh);
            snprintf(err, errlen, "failed to read filter file %s", path);
            return -1;
        }
        if (n == 0) {
            /* A last line without a newline still counts. */
            eof = 1;
            buf[n++] = '\n';
        }
        for (i = 0; i < (size_t)n; i++) {
            if (buf[i] != '\n' && buf[i] != '\r') {
                if (len < sizeof line - 1)
                    line[len++] = buf[i];
                else
                    too_long = 1;
                continue;
            }
            line[len] = '\0';
            if (too_long) {
                plat_close(fh);
                snprintf(err, errlen, "line too long in filter file %s", path);
                return -1;
            }
            if (len > 0 && line[0] != ';' && line[0] != '#'
                && filters_add(f, line, how, err, errlen) < 0) {
                plat_close(fh);
                return -1;
            }
            len = 0;
        }
    }
    plat_close(fh);
    return 0;
}

void filters_send(const ars_filters *f, ars_writer *w)
{
    const ars_filter_rule *r;
    size_t i, len;

    for (i = 0; i < f->count; i++) {
        r = &f->rules[i];
        len = strlen(r->pattern);
        w_int32(w, (int32_t)(2 + len + ((r->flags & FILTER_DIR_ONLY) ? 1 : 0)));
        w_bytes(w, (r->flags & FILTER_INCLUDE) ? "+ " : "- ", 2);
        w_bytes(w, r->pattern, len);
        if (r->flags & FILTER_DIR_ONLY)
            w_byte(w, '/');
    }
    w_int32(w, 0);
}
