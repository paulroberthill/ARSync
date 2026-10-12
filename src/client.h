/* client.h - the receiving side of a pull: file list, listing, transfer phases */
#ifndef CLIENT_H
#define CLIENT_H

#include "session.h"

/* Runs everything after session_setup_protocol(). With dest == NULL only
 * lists the files. Returns an RERR_* exit code. */
int client_pull(ars_session *s, const ars_options *o, const char *dest);

#endif
