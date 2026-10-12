/* push.h - the sending side: local files to an rsync daemon */
#ifndef PUSH_H
#define PUSH_H

#include "session.h"

/* Runs everything after session_setup_protocol() for a push of src.
 * Returns an RERR_* exit code. */
int client_push(ars_session *s, const ars_options *o, const char *src);

#endif
