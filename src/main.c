/* main.c - arsync command-line entry point */
#include <stdio.h>
#include <string.h>

#include "arsync.h"
#include "options.h"
#include "plat.h"
#include "session.h"
#include "url.h"

/* AmigaOS "version" command reads this. */
const char ars_version_tag[] = "$VER: arsync " ARSYNC_VERSION " (12.10.2026)";

static void usage(FILE *out)
{
    fprintf(out,
            "arsync " ARSYNC_VERSION " - rsync client for AmigaOS (protocol %d)\n"
            "\n"
            "Usage: arsync [OPTIONS] rsync://[USER@]HOST[:PORT]/MODULE[/PATH] DEST\n"
            "       arsync [OPTIONS] [USER@]HOST::MODULE[/PATH] DEST\n"
            "       arsync rsync://HOST/   or   arsync HOST::   (list modules)\n"
            "\n"
            "  -v, --verbose        increase verbosity\n"
            "      --port=PORT      daemon port (default 873)\n"
            "      --timeout=SECS   I/O timeout\n"
            "      --no-motd        suppress the daemon's message of the day\n"
            "  -h, --help           show this help\n"
            "      --version        show version\n",
            ARS_PROTOCOL_VERSION);
}

static int ars_main(int argc, char **argv)
{
    char err[256];
    ars_options opts;
    ars_remote rem;
    ars_session s;
    int rc;

    if (options_parse(&opts, argc, argv, err, sizeof err) < 0) {
        fprintf(stderr, "arsync: %s\n", err);
        fprintf(stderr, "arsync: use --help for usage\n");
        return RERR_SYNTAX;
    }
    if (opts.show_help) {
        usage(stdout);
        return RERR_OK;
    }
    if (opts.show_version) {
        printf("arsync %s, protocol version %d\n", ARSYNC_VERSION, ARS_PROTOCOL_VERSION);
        return RERR_OK;
    }
    if (opts.nargs == 0) {
        usage(stderr);
        return RERR_SYNTAX;
    }

    rc = parse_remote(opts.args[0], &rem, err, sizeof err);
    if (rc < 0) {
        fprintf(stderr, "arsync: %s\n", err);
        return RERR_SYNTAX;
    }
    if (rc == 0) {
        fprintf(stderr, "arsync: %s is not an rsync daemon location"
                " (use rsync://HOST/MODULE or HOST::MODULE)\n", opts.args[0]);
        return RERR_SYNTAX;
    }
    if (rem.path[0] != '\0') {
        fprintf(stderr, "arsync: transfers are not implemented yet\n");
        return RERR_UNSUPPORTED;
    }
    if (opts.nargs > 1) {
        fprintf(stderr, "arsync: listing modules takes no destination\n");
        return RERR_SYNTAX;
    }

    if ((rc = session_open(&s, &rem, &opts)) == RERR_OK)
        rc = session_list_modules(&s, &opts);
    session_close(&s);
    return rc;
}

int main(int argc, char **argv)
{
    return plat_run(ars_main, argc, argv);
}
