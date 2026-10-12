/* main.c - arsync command-line entry point */
#include <stdio.h>
#include <string.h>

#include "arsync.h"
#include "plat.h"

/* AmigaOS "version" command reads this. */
const char ars_version_tag[] = "$VER: arsync " ARSYNC_VERSION " (12.10.2026)";

static void usage(FILE *out)
{
    fprintf(out,
            "arsync " ARSYNC_VERSION " - rsync client for AmigaOS (protocol %d)\n"
            "\n"
            "Usage: arsync [OPTIONS] rsync://HOST[:PORT]/MODULE[/PATH] DEST\n"
            "       arsync [OPTIONS] HOST::MODULE[/PATH] DEST\n"
            "       arsync rsync://HOST/          (list modules)\n"
            "\n"
            "  -h, --help      show this help\n"
            "      --version   show version\n",
            ARS_PROTOCOL_VERSION);
}

static int ars_main(int argc, char **argv)
{
    if (argc < 2) {
        usage(stderr);
        return RERR_SYNTAX;
    }
    if (strcmp(argv[1], "--version") == 0) {
        printf("arsync %s, protocol version %d\n", ARSYNC_VERSION, ARS_PROTOCOL_VERSION);
        return RERR_OK;
    }
    if (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0) {
        usage(stdout);
        return RERR_OK;
    }
    fprintf(stderr, "arsync: transfers are not implemented yet\n");
    return RERR_UNSUPPORTED;
}

int main(int argc, char **argv)
{
    return plat_run(ars_main, argc, argv);
}
