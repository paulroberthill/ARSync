/* session.h - rsync daemon session: connect, greeting, module requests,
 * authentication and the protocol setup that precedes a transfer */
#ifndef SESSION_H
#define SESSION_H

#include <time.h>

#include "net.h"
#include "options.h"
#include "url.h"

/* Digests we can use, best first. The auth list goes in our greeting
 * (mandatory from protocol 32); the checksum list is negotiated after the
 * server arguments. */
#define ARS_AUTH_DIGESTS "md5 md4"
#define ARS_CHECKSUMS "md5 md4"

/* Capability letters sent to the server in "-e.<letters>":
 *   f  safe file list (I/O-error end marker)
 *   x  avoid the xattr hardlink optimisation
 *   C  checksum-seed order fix
 *   I  inplace/partial-dir compatibility
 *   v  varint file-list flags, negotiated checksum/compression
 *   u  send uid/gid 0 names
 * Deliberately absent: i (incremental recursion), L and s (symlinks). */
#define ARS_CLIENT_INFO "fxCIvu"

/* Compatibility flags the server sends back. */
#define CF_INC_RECURSE (1 << 0)
#define CF_SYMLINK_TIMES (1 << 1)
#define CF_SYMLINK_ICONV (1 << 2)
#define CF_SAFE_FLIST (1 << 3)
#define CF_AVOID_XATTR_OPTIM (1 << 4)
#define CF_CHKSUM_SEED_FIX (1 << 5)
#define CF_INPLACE_PARTIAL_DIR (1 << 6)
#define CF_VARINT_FLIST_FLAGS (1 << 7)
#define CF_ID0_NAMES (1 << 8)

#define SESSION_MAX_ARGS 16

typedef struct ars_session {
    time_t started;             /* for the transfer rate */
    ars_conn conn;
    int connected;
    int remote_protocol;
    int remote_sub;
    int protocol;               /* negotiated */
    char remote_digests[128];   /* server's auth digest list, may be empty */

    /* Set up by session_setup_protocol(). */
    int32_t compat_flags;
    int checksum_type;          /* DIGEST_* for block and file checksums */
    int32_t checksum_seed;

    /* Multiplexed streams, valid once multiplexing has started. */
    ars_demux demux;
    ars_mux mux;
    int multiplexing;
    ars_reader in;              /* server data, messages handled */
    ars_writer out;             /* our data to the server */

    /* Out-of-band state reported by the server. */
    int remote_io_error;
    int remote_exit;            /* from MSG_ERROR_EXIT, -1 if none */
    int saw_error;              /* server reported an error message */
    int saw_xfer_error;         /* ...one about a file (makes the exit code 23) */
    int no_send_pending;        /* MSG_NO_SEND notices not yet accounted for:
                                 * files the server couldn't send */
    int awaiting_reply;         /* inside session_wait_reply() */
} ars_session;

/* Picks the protocol version both sides speak, mirroring rsync's rule: the
 * lower of the two, minus one when the side with that version is running a
 * pre-release (non-zero subprotocol) the other can't know about. */
int negotiate_protocol(int ours, int our_sub, int remote, int remote_sub);

/* Parses a daemon greeting line ("@RSYNCD: 32.0 sha512 md5"). Returns 0 or
 * -1 if it isn't one. */
int parse_greeting(const char *line, int *protocol, int *sub, char *digests, size_t digestsize);

/* Builds the argument list a pulling client sends to the daemon: the server
 * runs as sender of rem->path. Returns the count; strings point into buf
 * or at constants. */
int build_server_args(const ars_options *o, const ars_remote *rem,
                      const char **args, char *buf, size_t bufsize);

/* Each of these returns an RERR_* exit code and prints its own errors. */
int session_open(ars_session *s, const ars_remote *rem, const ars_options *o);
int session_list_modules(ars_session *s, const ars_options *o);
/* Asks for rem's module, authenticates if required, and sends args. */
int session_request(ars_session *s, const ars_remote *rem, const ars_options *o,
                    const char **args, int nargs);
/* Reads compat flags, negotiates checksums, reads the seed, and starts
 * multiplexing in both directions. */
int session_setup_protocol(ars_session *s, const ars_options *o);
/* Waits for the next reply to start, handling messages in front of it.
 * Returns WIRE_OK, WIRE_EINTR if a MSG_NO_SEND arrived (one reply fewer is
 * coming), or a stream error. */
int session_wait_reply(ars_session *s);
/* Maps a failure on s->in / s->out onto an exit code, after reporting it. */
int session_stream_failure(ars_session *s, const char *what);
void session_close(ars_session *s);

#endif
