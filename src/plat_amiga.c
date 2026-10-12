/* plat_amiga.c - platform layer for AmigaOS 3.x
 *
 * Talks to dos.library and bsdsocket.library directly rather than through
 * the C runtime, so Ctrl-C, DateStamps and protection bits are handled the
 * Amiga way.
 *
 * Library bases: SocketBase is not in the startup code's auto-open table
 * (checked: declaring it pulls in no auto-open). LocaleBase *is* in the
 * table, so it's renamed via LOCALE_BASE_NAME and opened here instead,
 * which lets a missing locale.library degrade to GMT rather than abort.
 */
#include <sys/types.h>
#include <string.h>

#include <exec/memory.h>
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/bsdsocket.h>

#define __NOLIBBASE__
#define LOCALE_BASE_NAME ArsLocaleBase
#include <proto/locale.h>
#undef __NOLIBBASE__

#include "plat.h"
#include "amitime.h"

struct Library *SocketBase;
struct Library *ArsLocaleBase;   /* NOT "LocaleBase" - see above */

/* bsdsocket.library reports BSD errno values, which differ from the C
 * runtime's <errno.h>, so the few we need are spelled out here. */
#define BSD_EINTR 4
#define BSD_EWOULDBLOCK 35

/* The NDK ships no <sys/filio.h>; this is BSD's _IOW('f', 126, int). */
#ifndef FIONBIO
#define FIONBIO 0x8004667EUL
#endif

struct plat_file {
    BPTR fh;
};

struct plat_dir {
    BPTR lock;
    struct FileInfoBlock *fib;
};

static long gmt_offset;     /* minutes west of GMT, from locale.library */
static int got_break;

/* --- startup, stack, Ctrl-C --- */

/* Everything the stack-swapped call touches must be static: once
 * StackSwap() returns we're on the new stack, and anything the compiler had
 * kept in the old stack frame is out of reach. */
static struct StackSwapStruct swap;
static int (*run_fn)(int, char **);
static int run_argc;
static char **run_argv;
static int run_rc;

static void __attribute__((noinline)) run_on_new_stack(void)
{
    StackSwap(&swap);
    run_rc = run_fn(run_argc, run_argv);
    StackSwap(&swap);
}

static void read_gmt_offset(void)
{
    struct Locale *loc;

    ArsLocaleBase = OpenLibrary((CONST_STRPTR)"locale.library", 38);
    if (!ArsLocaleBase)
        return;
    if ((loc = OpenLocale(NULL)) != NULL) {
        gmt_offset = loc->loc_GMTOffset;
        CloseLocale(loc);
    }
    CloseLibrary(ArsLocaleBase);
    ArsLocaleBase = NULL;
}

int plat_run(int (*fn)(int argc, char **argv), int argc, char **argv)
{
    APTR stack;

    read_gmt_offset();

    run_fn = fn;
    run_argc = argc;
    run_argv = argv;
    stack = AllocVec(ARS_STACK_SIZE, MEMF_ANY);
    if (stack) {
        swap.stk_Lower = stack;
        swap.stk_Upper = (ULONG)stack + ARS_STACK_SIZE;
        swap.stk_Pointer = (APTR)swap.stk_Upper;
        run_on_new_stack();
        FreeVec(stack);
    } else {
        run_rc = fn(argc, argv);   /* fall back to whatever stack we have */
    }

    if (SocketBase) {
        CloseLibrary(SocketBase);
        SocketBase = NULL;
    }
    return run_rc;
}

int plat_break_pending(void)
{
    if (!got_break && (SetSignal(0, SIGBREAKF_CTRL_C) & SIGBREAKF_CTRL_C))
        got_break = 1;
    return got_break;
}

int plat_read_password(const char *prompt, char *buf, size_t bufsize)
{
    BPTR in = Input(), out = Output();
    int raw = IsInteractive(in) != 0;
    size_t len = 0;
    int rc = PLAT_OK;
    char ch;

    Write(out, (APTR)prompt, (LONG)strlen(prompt));
    /* Raw mode stops the console echoing what's typed; we then handle
     * line editing ourselves. Non-interactive input is read as is. */
    if (raw)
        SetMode(in, 1);
    for (;;) {
        if (Read(in, &ch, 1) != 1) {
            rc = PLAT_EIO;
            break;
        }
        if (ch == '\r' || ch == '\n')
            break;
        if (ch == 3) {                      /* Ctrl-C arrives as a character in raw mode */
            got_break = 1;
            rc = PLAT_EBREAK;
            break;
        }
        if (ch == 8 || ch == 127) {         /* backspace / delete */
            if (len > 0)
                len--;
        } else if (len + 1 < bufsize && (unsigned char)ch >= 32) {
            buf[len++] = ch;
        }
    }
    if (raw)
        SetMode(in, 0);
    Write(out, "\n", 1);
    buf[len] = '\0';
    if (plat_break_pending())
        rc = PLAT_EBREAK;
    return rc;
}

void plat_localtime(int64_t t, ars_tm *tm)
{
    secs_to_civil(t - (int64_t)gmt_offset * 60, tm);
}

/* --- sockets --- */

static int open_net(void)
{
    if (!SocketBase)
        SocketBase = OpenLibrary((CONST_STRPTR)"bsdsocket.library", 4);
    return SocketBase ? PLAT_OK : PLAT_ENONET;
}

int plat_tcp_connect(const char *host, int port, int *sock_out)
{
    struct sockaddr_in sin;
    struct hostent *he;
    LONG one = 1;
    int s, rc;

    if ((rc = open_net()) != PLAT_OK)
        return rc;

    memset(&sin, 0, sizeof sin);
    sin.sin_len = sizeof sin;
    sin.sin_family = AF_INET;
    sin.sin_port = htons((unsigned short)port);
    sin.sin_addr.s_addr = inet_addr((STRPTR)host);
    if (sin.sin_addr.s_addr == INADDR_NONE) {
        if (!(he = gethostbyname((STRPTR)host)) || he->h_addrtype != AF_INET)
            return plat_break_pending() ? PLAT_EBREAK : PLAT_EHOST;
        memcpy(&sin.sin_addr, he->h_addr_list[0], sizeof sin.sin_addr);
    }

    if ((s = socket(AF_INET, SOCK_STREAM, 0)) < 0)
        return PLAT_EIO;
    /* A blocking connect() is interrupted by Ctrl-C: the stacks' default
     * break mask is SIGBREAKF_CTRL_C. */
    if (connect(s, (struct sockaddr *)&sin, sizeof sin) < 0) {
        rc = Errno() == BSD_EINTR || plat_break_pending() ? PLAT_EBREAK : PLAT_ECONN;
        CloseSocket(s);
        return rc;
    }
    if (IoctlSocket(s, FIONBIO, (char *)&one) < 0) {
        CloseSocket(s);
        return PLAT_EIO;
    }
    *sock_out = s;
    return PLAT_OK;
}

static long sock_error(void)
{
    LONG e = Errno();

    return e == BSD_EWOULDBLOCK || e == BSD_EINTR ? PLAT_EAGAIN : PLAT_EIO;
}

long plat_sock_send(int sock, const void *buf, size_t n)
{
    LONG r = send(sock, (APTR)buf, (LONG)n, 0);

    return r >= 0 ? r : sock_error();
}

long plat_sock_recv(int sock, void *buf, size_t n)
{
    LONG r = recv(sock, buf, (LONG)n, 0);

    return r >= 0 ? r : sock_error();
}

int plat_sock_wait(int sock, int want_read, int want_write, long timeout_ms,
                   int *readable, int *writable)
{
    fd_set rfds, wfds;
    struct timeval tv;
    ULONG sigs = SIGBREAKF_CTRL_C;
    LONG r;

    *readable = *writable = 0;
    if (plat_break_pending())
        return PLAT_EBREAK;
    FD_ZERO(&rfds);
    FD_ZERO(&wfds);
    if (want_read)
        FD_SET(sock, &rfds);
    if (want_write)
        FD_SET(sock, &wfds);
    if (timeout_ms >= 0) {
        tv.tv_sec = timeout_ms / 1000;
        tv.tv_usec = timeout_ms % 1000 * 1000;
    }
    r = WaitSelect(sock + 1, &rfds, &wfds, NULL, timeout_ms >= 0 ? &tv : NULL, &sigs);
    if (r < 0)
        return PLAT_EIO;
    if (sigs & SIGBREAKF_CTRL_C) {
        got_break = 1;
        return PLAT_EBREAK;
    }
    if (r == 0)
        return PLAT_ETIMEOUT;
    *readable = FD_ISSET(sock, &rfds) != 0;
    *writable = FD_ISSET(sock, &wfds) != 0;
    return PLAT_OK;
}

void plat_sock_close(int sock)
{
    if (SocketBase)
        CloseSocket(sock);
}

/* --- files --- */

static int map_ioerr(LONG err)
{
    switch (err) {
    case 0:
        return PLAT_EIO;
    case ERROR_OBJECT_NOT_FOUND:
    case ERROR_DIR_NOT_FOUND:
        return PLAT_ENOENT;
    case ERROR_OBJECT_EXISTS:
    case ERROR_DIRECTORY_NOT_EMPTY:
        return PLAT_EEXIST;
    case ERROR_NO_FREE_STORE:
        return PLAT_ENOMEM;
    case ERROR_BREAK:
        return PLAT_EBREAK;
    default:
        return PLAT_EIO;
    }
}

static int open_file(const char *path, LONG mode, plat_file **out)
{
    plat_file *f;
    BPTR fh = Open((CONST_STRPTR)path, mode);

    if (!fh)
        return map_ioerr(IoErr());
    if (!(f = AllocVec(sizeof *f, MEMF_ANY))) {
        Close(fh);
        return PLAT_ENOMEM;
    }
    f->fh = fh;
    *out = f;
    return PLAT_OK;
}

int plat_open_read(const char *path, plat_file **out)
{
    return open_file(path, MODE_OLDFILE, out);
}

int plat_open_write(const char *path, plat_file **out)
{
    return open_file(path, MODE_NEWFILE, out);
}

long plat_read(plat_file *f, void *buf, size_t n)
{
    LONG r = Read(f->fh, buf, (LONG)n);

    return r < 0 ? map_ioerr(IoErr()) : r;
}

int plat_write(plat_file *f, const void *buf, size_t n)
{
    return Write(f->fh, (APTR)buf, (LONG)n) == (LONG)n ? PLAT_OK : map_ioerr(IoErr());
}

int plat_seek(plat_file *f, int64_t offset)
{
    if (offset < 0 || offset > 0x7FFFFFFF)
        return PLAT_ETOOBIG;
    /* Seek() returns the old position, which can legitimately be -1 only on
     * error; OS 3.x doesn't always set IoErr() on success, so clear it. */
    SetIoErr(0);
    if (Seek(f->fh, (LONG)offset, OFFSET_BEGINNING) < 0 && IoErr())
        return map_ioerr(IoErr());
    return PLAT_OK;
}

int plat_close(plat_file *f)
{
    int rc = Close(f->fh) ? PLAT_OK : map_ioerr(IoErr());

    FreeVec(f);
    return rc;
}

static void fill_stat(const struct FileInfoBlock *fib, plat_fileinfo *st)
{
    ars_amidate d;

    st->type = fib->fib_DirEntryType > 0 ? PLAT_T_DIR : PLAT_T_FILE;
    st->size = (int64_t)(ULONG)fib->fib_Size;
    d.days = fib->fib_Date.ds_Days;
    d.minute = fib->fib_Date.ds_Minute;
    d.tick = fib->fib_Date.ds_Tick;
    st->mtime = amidate_to_unix(&d, gmt_offset, &st->mtime_nsec);
}

int plat_stat(const char *path, plat_fileinfo *st)
{
    struct FileInfoBlock *fib;
    BPTR lock;
    int rc = PLAT_OK;

    if (!(lock = Lock((CONST_STRPTR)path, SHARED_LOCK)))
        return map_ioerr(IoErr());
    if (!(fib = AllocDosObject(DOS_FIB, NULL))) {
        UnLock(lock);
        return PLAT_ENOMEM;
    }
    if (Examine(lock, fib))
        fill_stat(fib, st);
    else
        rc = map_ioerr(IoErr());
    FreeDosObject(DOS_FIB, fib);
    UnLock(lock);
    return rc;
}

int plat_mkdir(const char *path)
{
    BPTR lock = CreateDir((CONST_STRPTR)path);

    if (!lock)
        return map_ioerr(IoErr());
    UnLock(lock);
    return PLAT_OK;
}

int plat_remove(const char *path)
{
    return DeleteFile((CONST_STRPTR)path) ? PLAT_OK : map_ioerr(IoErr());
}

int plat_rename(const char *from, const char *to)
{
    if (Rename((CONST_STRPTR)from, (CONST_STRPTR)to))
        return PLAT_OK;
    if (IoErr() != ERROR_OBJECT_EXISTS)
        return map_ioerr(IoErr());
    /* AmigaDOS won't rename over an existing file, so this replacement is
     * not atomic: a crash in between leaves only the temp file. */
    if (!DeleteFile((CONST_STRPTR)to))
        return map_ioerr(IoErr());
    return Rename((CONST_STRPTR)from, (CONST_STRPTR)to) ? PLAT_OK : map_ioerr(IoErr());
}

int plat_set_mtime(const char *path, int64_t mtime, long nsec)
{
    struct DateStamp ds;
    ars_amidate d;

    unix_to_amidate(mtime, nsec, gmt_offset, &d);
    ds.ds_Days = d.days;
    ds.ds_Minute = d.minute;
    ds.ds_Tick = d.tick;
    return SetFileDate((CONST_STRPTR)path, &ds) ? PLAT_OK : map_ioerr(IoErr());
}

int plat_new_file_perms(const char *path, uint32_t wire_mode, const char *existing)
{
    struct FileInfoBlock *fib;
    BPTR lock;
    int rc = PLAT_OK;

    (void)wire_mode;
    if (!existing || !(lock = Lock((CONST_STRPTR)existing, SHARED_LOCK)))
        return PLAT_OK;
    if ((fib = AllocDosObject(DOS_FIB, NULL)) != NULL) {
        if (Examine(lock, fib) && !SetProtection((CONST_STRPTR)path, fib->fib_Protection))
            rc = map_ioerr(IoErr());
        FreeDosObject(DOS_FIB, fib);
    }
    UnLock(lock);
    return rc;
}

int plat_opendir(const char *path, plat_dir **out)
{
    plat_dir *d;
    int rc;

    if (!(d = AllocVec(sizeof *d, MEMF_ANY | MEMF_CLEAR)))
        return PLAT_ENOMEM;
    if (!(d->lock = Lock((CONST_STRPTR)path, SHARED_LOCK))) {
        rc = map_ioerr(IoErr());
        goto fail;
    }
    if (!(d->fib = AllocDosObject(DOS_FIB, NULL))) {
        rc = PLAT_ENOMEM;
        goto fail;
    }
    if (!Examine(d->lock, d->fib)) {
        rc = map_ioerr(IoErr());
        goto fail;
    }
    if (d->fib->fib_DirEntryType <= 0) {
        rc = PLAT_ENOENT;    /* not a directory */
        goto fail;
    }
    *out = d;
    return PLAT_OK;

fail:
    plat_closedir(d);
    return rc;
}

int plat_readdir(plat_dir *d, char *name, size_t namesize)
{
    if (!ExNext(d->lock, d->fib))
        return IoErr() == ERROR_NO_MORE_ENTRIES ? 0 : map_ioerr(IoErr());
    if (strlen((char *)d->fib->fib_FileName) >= namesize)
        return PLAT_ETOOBIG;
    strcpy(name, (char *)d->fib->fib_FileName);
    return 1;
}

void plat_closedir(plat_dir *d)
{
    if (d->fib)
        FreeDosObject(DOS_FIB, d->fib);
    if (d->lock)
        UnLock(d->lock);
    FreeVec(d);
}
