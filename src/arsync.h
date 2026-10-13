/* arsync.h - project-wide definitions */
#ifndef ARSYNC_H
#define ARSYNC_H

#include <stddef.h>
#include <stdint.h>

#define ARSYNC_VERSION "0.3.0"

#define ARS_PROTOCOL_VERSION 32
#define ARS_MIN_PROTOCOL 30

/* Exit codes, numerically identical to rsync's so scripts can treat
 * arsync like rsync. */
enum {
    RERR_OK = 0,
    RERR_SYNTAX = 1,
    RERR_PROTOCOL = 2,
    RERR_FILESELECT = 3,
    RERR_UNSUPPORTED = 4,
    RERR_STARTCLIENT = 5,
    RERR_SOCKETIO = 10,
    RERR_FILEIO = 11,
    RERR_STREAMIO = 12,
    RERR_MESSAGEIO = 13,
    RERR_SIGNAL = 20,
    RERR_MALLOC = 22,
    RERR_PARTIAL = 23,
    RERR_VANISHED = 24,
    RERR_TIMEOUT = 30,
    RERR_CONTIMEOUT = 35
};

#endif
