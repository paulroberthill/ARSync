/* plat.h - operating-system abstraction
 *
 * Everything that differs between AmigaOS and the POSIX host build lives
 * behind this interface: plat_amiga.c uses dos.library and bsdsocket.library
 * directly, plat_posix.c uses POSIX calls. The rest of arsync is portable C.
 *
 * Functions return PLAT_OK (0) or a negative PLAT_E* code unless noted.
 */
#ifndef PLAT_H
#define PLAT_H

#include "arsync.h"

#define PLAT_OK 0
#define PLAT_EIO (-1)
#define PLAT_EAGAIN (-2)     /* non-blocking socket op would block */
#define PLAT_EBREAK (-3)     /* Ctrl-C / SIGINT */
#define PLAT_ETIMEOUT (-4)
#define PLAT_ENOENT (-5)
#define PLAT_EEXIST (-6)
#define PLAT_ENOMEM (-7)
#define PLAT_ENONET (-8)     /* no TCP/IP stack available */
#define PLAT_EHOST (-9)      /* host name lookup failed */
#define PLAT_ECONN (-10)     /* connection refused or unreachable */
#define PLAT_ETOOBIG (-11)   /* offset or size beyond what the OS supports */

const char *plat_strerror(int err);

/* Initialise the platform, run fn, clean up, and return fn's result.
 * On AmigaOS this also moves fn onto a private stack of ARS_STACK_SIZE
 * bytes, since the default CLI stack (often 4 KiB) is too small. */
#define ARS_STACK_SIZE 65536
int plat_run(int (*fn)(int argc, char **argv), int argc, char **argv);

/* Non-zero once Ctrl-C (AmigaOS) or SIGINT (POSIX) has been seen. Sticky. */
int plat_break_pending(void);

/* Prompts on the console and reads a line without echoing it. */
int plat_read_password(const char *prompt, char *buf, size_t bufsize);

/* Converts Unix time to local calendar time. */
struct ars_tm;
void plat_localtime(int64_t t, struct ars_tm *tm);

/* --- TCP sockets --- */

/* Resolves host (name or dotted quad), connects, and leaves the socket in
 * non-blocking mode. */
int plat_tcp_connect(const char *host, int port, int *sock_out);
/* Return bytes transferred (recv: 0 means EOF), PLAT_EAGAIN or PLAT_EIO. */
long plat_sock_send(int sock, const void *buf, size_t n);
long plat_sock_recv(int sock, void *buf, size_t n);
/* Waits until the socket is readable and/or writable (as requested), the
 * timeout (ms, < 0 = forever) expires, or Ctrl-C arrives. */
int plat_sock_wait(int sock, int want_read, int want_write, long timeout_ms,
                   int *readable, int *writable);
void plat_sock_close(int sock);

/* --- files --- */

typedef struct plat_file plat_file;

int plat_open_read(const char *path, plat_file **out);
int plat_open_write(const char *path, plat_file **out);   /* create/truncate */
/* Returns bytes read (0 at end of file) or PLAT_EIO. */
long plat_read(plat_file *f, void *buf, size_t n);
int plat_write(plat_file *f, const void *buf, size_t n);  /* writes all n */
int plat_seek(plat_file *f, int64_t offset);              /* from start */
int plat_close(plat_file *f);

#define PLAT_T_FILE 1
#define PLAT_T_DIR 2
#define PLAT_T_OTHER 3

typedef struct plat_fileinfo {
    int type;
    int64_t size;
    int64_t mtime;     /* Unix seconds, UTC */
    long mtime_nsec;
} plat_fileinfo;

int plat_stat(const char *path, plat_fileinfo *st);
int plat_mkdir(const char *path);                      /* PLAT_EEXIST if present */
int plat_remove(const char *path);                     /* file or empty dir */
int plat_rename(const char *from, const char *to);     /* replaces `to` */
int plat_set_mtime(const char *path, int64_t mtime, long nsec);

typedef struct plat_dir plat_dir;

int plat_opendir(const char *path, plat_dir **out);
/* Returns 1 with the next name, 0 at the end, or a PLAT_E* code.
 * Never returns "." or "..". */
int plat_readdir(plat_dir *d, char *name, size_t namesize);
void plat_closedir(plat_dir *d);

#endif
