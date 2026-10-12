/* url.h - parsing rsync daemon locations
 *
 * Accepted forms (matching rsync):
 *   rsync://[USER@]HOST[:PORT][/MODULE[/PATH]]
 *   [USER@]HOST::[MODULE[/PATH]]
 * Anything else is a local path. A single colon is *not* a remote-shell spec
 * here: arsync has no remote-shell transport, and AmigaDOS paths such as
 * "Work:dir" contain one.
 */
#ifndef URL_H
#define URL_H

#include "arsync.h"

#define RSYNC_PORT 873

typedef struct ars_remote {
    char user[64];      /* empty if not given */
    char host[256];
    int port;           /* 0 if not given in the location */
    char path[1024];    /* "MODULE/PATH" as given, may be empty */
} ars_remote;

/* Returns 1 and fills *rem for a daemon location, 0 for a local path, or
 * -1 for a malformed daemon location (with a message in err). */
int parse_remote(const char *arg, ars_remote *rem, char *err, size_t errlen);

/* Copies the module name (path up to the first '/') into buf. */
void remote_module(const ars_remote *rem, char *buf, size_t bufsize);

#endif
