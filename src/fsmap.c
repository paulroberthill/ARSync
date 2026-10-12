/* fsmap.c - which incoming files the local filesystem can hold */
#include <stdlib.h>
#include <string.h>

#include "fsmap.h"

const char *fsmap_check_name(const ars_fsrules *r, const char *path)
{
    const char *base = strrchr(path, '/');

    base = base ? base + 1 : path;
    if (strcmp(path, ".") == 0)
        return NULL;
    if (strchr(base, ':'))
        return "names can't contain ':' here";
    if (r->name_max > 0 && strlen(base) > (size_t)r->name_max)
        return "name too long for this filesystem (see --max-name)";
    return NULL;
}

/* Only ASCII letters are folded: AmigaDOS's international mode would also
 * fold Latin-1, but most names now arrive as UTF-8, whose bytes must not
 * be folded as if they were Latin-1 letters. */
static int fold(int c)
{
    return c >= 'a' && c <= 'z' ? c - 'a' + 'A' : c;
}

static int fold_cmp(const char *a, const char *b)
{
    int ca, cb;

    for (;; a++, b++) {
        ca = fold((unsigned char)*a);
        cb = fold((unsigned char)*b);
        if (ca != cb || !ca)
            return ca - cb;
    }
}

static const ars_flist *sort_fl;

static int by_folded_name(const void *x, const void *y)
{
    size_t a = *(const size_t *)x, b = *(const size_t *)y;
    int c = fold_cmp(flist_name(sort_fl, &sort_fl->files[a]), flist_name(sort_fl, &sort_fl->files[b]));

    return c ? c : (a > b) - (a < b);
}

long fsmap_find_case_clashes(const ars_flist *fl, uint8_t *flags, uint8_t mark)
{
    size_t *idx, n = 0, i;
    long clashes = 0;

    if (!(idx = malloc((fl->count ? fl->count : 1) * sizeof *idx)))
        return -1;
    for (i = 0; i < fl->count; i++)
        if (fl->files[i].mode)
            idx[n++] = i;

    sort_fl = fl;
    qsort(idx, n, sizeof *idx, by_folded_name);
    /* Within each run of equal folded names, all but the first (lowest
     * index) clash. */
    for (i = 1; i < n; i++) {
        if (fold_cmp(flist_name(fl, &fl->files[idx[i - 1]]), flist_name(fl, &fl->files[idx[i]])) == 0) {
            flags[idx[i]] |= mark;
            clashes++;
        }
    }
    free(idx);
    return clashes;
}
