/* session.h - rsync daemon session: connect, greeting, module requests */
#ifndef SESSION_H
#define SESSION_H

#include "net.h"
#include "options.h"
#include "url.h"

/* Digests we can use for daemon authentication, best first. Sent in our
 * greeting; mandatory from protocol 32. */
#define ARS_AUTH_DIGESTS "md5 md4"

typedef struct ars_session {
    ars_conn conn;
    int connected;
    int remote_protocol;
    int remote_sub;
    int protocol;               /* negotiated */
    char remote_digests[128];   /* server's auth digest list, may be empty */
} ars_session;

/* Picks the protocol version both sides speak, mirroring rsync's rule: the
 * lower of the two, minus one when the side with that version is running a
 * pre-release (non-zero subprotocol) the other can't know about. */
int negotiate_protocol(int ours, int our_sub, int remote, int remote_sub);

/* Parses a daemon greeting line ("@RSYNCD: 32.0 sha512 md5"). Returns 0 or
 * -1 if it isn't one. */
int parse_greeting(const char *line, int *protocol, int *sub, char *digests, size_t digestsize);

/* Each returns an RERR_* exit code and prints its own error messages. */
int session_open(ars_session *s, const ars_remote *rem, const ars_options *o);
int session_list_modules(ars_session *s, const ars_options *o);
void session_close(ars_session *s);

#endif
