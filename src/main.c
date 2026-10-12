/* main.c - arsync command-line entry point */
#include <stdio.h>
#include <string.h>

#include "arsync.h"
#include "options.h"
#include "plat.h"
#include "client.h"
#include "push.h"
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
            "       arsync [OPTIONS] SRC rsync://[USER@]HOST[:PORT]/MODULE[/PATH]   (push)\n"
            "       arsync [OPTIONS] SRC [USER@]HOST::MODULE[/PATH]   (push)\n"
            "       arsync [OPTIONS] rsync://HOST/MODULE[/PATH]   (list files)\n"
            "       arsync rsync://HOST/   or   arsync HOST::   (list modules)\n"
            "\n"
            "  -v, --verbose        increase verbosity\n"
            "  -r, --recursive      recurse into directories\n"
            "  -t, --times          preserve modification times\n"
            "  -a, --archive        same as -rt (arsync has no links, perms, owners)\n"
            "  -n, --dry-run        show what would be transferred\n"
            "  -W, --whole-file     send whole files (no delta transfer)\n"
            "  -P, --progress       show progress during transfer\n"
            "      --max-name=N     longest file name the destination takes\n"
            "                       (default: detected; 30 on FFS)\n"
            "      --amiga-rules    apply AmigaDOS limits on other systems (testing)\n"
            "      --list-only      list the files instead of copying them\n"
            "      --port=PORT      daemon port (default 873)\n"
            "      --timeout=SECS   I/O timeout\n"
            "      --no-motd        suppress the daemon's message of the day\n"
            "      --password-file=FILE  read the daemon password from FILE\n"
            "  -h, --help           show this help\n"
            "      --version        show version\n",
            ARS_PROTOCOL_VERSION);
}

/* Runs a transfer with the daemon at rem: fetching rem's path, or (push)
 * sending local path src to it. */
static int transfer(const ars_remote *rem, const ars_options *opts, int push, const char *src)
{
    const char *args[SESSION_MAX_ARGS];
    char argbuf[512];
    ars_session s;
    int nargs, rc;

    nargs = build_server_args(opts, rem, push, args, argbuf, sizeof argbuf);
    rc = session_open(&s, rem, opts);
    if (rc == RERR_OK)
        rc = session_request(&s, rem, opts, args, nargs);
    if (rc == RERR_OK)
        rc = session_setup_protocol(&s, opts, push);
    if (rc == RERR_OK) {
        if (push)
            rc = client_push(&s, opts, src);
        else
            rc = client_pull(&s, opts, opts->list_only ? NULL : opts->args[1]);
    }
    session_close(&s);
    return rc;
}

static int ars_main(int argc, char **argv)
{
    char err[256];
    ars_options opts;
    ars_remote rem, dummy;
    ars_session s;
    int rc;

    /* Keep stdout (MOTD, server messages) in order with stderr errors. */
    setvbuf(stdout, NULL, _IOLBF, BUFSIZ);

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
        /* A local source: this is a push, to a daemon location. */
        if (opts.nargs == 2) {
            rc = parse_remote(opts.args[1], &rem, err, sizeof err);
            if (rc < 0) {
                fprintf(stderr, "arsync: %s\n", err);
                return RERR_SYNTAX;
            }
            if (rc == 1) {
                if (opts.list_only) {
                    fprintf(stderr, "arsync: --list-only only lists remote files\n");
                    return RERR_SYNTAX;
                }
                return transfer(&rem, &opts, 1, opts.args[0]);
            }
        }
        fprintf(stderr, "arsync: one of %s must be an rsync daemon location"
                " (rsync://HOST/MODULE or HOST::MODULE); arsync doesn't copy locally\n",
                opts.nargs == 2 ? "the arguments" : opts.args[0]);
        return RERR_SYNTAX;
    }
    if (opts.nargs == 2 && parse_remote(opts.args[1], &dummy, err, sizeof err) == 1) {
        fprintf(stderr, "arsync: copying between two daemons isn't supported\n");
        return RERR_SYNTAX;
    }
    if (rem.path[0] == '\0') {
        if (opts.nargs > 1) {
            fprintf(stderr, "arsync: listing modules takes no destination\n");
            return RERR_SYNTAX;
        }
        if ((rc = session_open(&s, &rem, &opts)) == RERR_OK)
            rc = session_list_modules(&s, &opts);
        session_close(&s);
        return rc;
    }

    /* Without a destination, rsync lists the source. */
    if (opts.nargs == 1 && !opts.list_only)
        opts.list_only = 1;
    return transfer(&rem, &opts, 0, NULL);
}

int main(int argc, char **argv)
{
    return plat_run(ars_main, argc, argv);
}
