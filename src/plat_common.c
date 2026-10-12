/* plat_common.c - platform-independent parts of the platform layer */
#include "plat.h"

const char *plat_strerror(int err)
{
    switch (err) {
    case PLAT_OK: return "no error";
    case PLAT_EAGAIN: return "operation would block";
    case PLAT_EBREAK: return "interrupted by user";
    case PLAT_ETIMEOUT: return "timed out";
    case PLAT_ENOENT: return "object not found";
    case PLAT_EEXIST: return "object already exists";
    case PLAT_ENOMEM: return "out of memory";
    case PLAT_ENONET: return "no TCP/IP stack available";
    case PLAT_EHOST: return "host name lookup failed";
    case PLAT_ECONN: return "connection failed";
    case PLAT_ETOOBIG: return "value too large for this system";
    default: return "I/O error";
    }
}
