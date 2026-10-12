/* plat_posix.c - platform layer for the POSIX host build */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
#include <netinet/in.h>

#include "plat.h"
#include "amitime.h"

struct plat_file {
    int fd;
};

struct plat_dir {
    DIR *dir;
    char path[4096];
};

static volatile sig_atomic_t got_break;
static mode_t saved_umask = 022;

static void on_sigint(int sig)
{
    (void)sig;
    got_break = 1;
}

static int map_errno(int e)
{
    switch (e) {
    case ENOENT:
    case ENOTDIR:
        return PLAT_ENOENT;
    case EEXIST:
    case ENOTEMPTY:
        return PLAT_EEXIST;
    case ENOMEM:
        return PLAT_ENOMEM;
    case EACCES:
    case EPERM:
    case EROFS:
        return PLAT_EACCES;
    case EFBIG:
    case EOVERFLOW:
        return PLAT_ETOOBIG;
    case EINTR:
        return got_break ? PLAT_EBREAK : PLAT_EIO;
    default:
        return PLAT_EIO;
    }
}

int plat_run(int (*fn)(int argc, char **argv), int argc, char **argv)
{
    struct sigaction sa;

    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_sigint;   /* no SA_RESTART: let poll() return EINTR */
    sigaction(SIGINT, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);
    saved_umask = umask(0);
    umask(saved_umask);
    return fn(argc, argv);
}

int plat_break_pending(void)
{
    return got_break;
}

int plat_read_password(const char *prompt, char *buf, size_t bufsize)
{
    struct termios old, quiet;
    FILE *tty = fopen("/dev/tty", "r+");
    FILE *in = tty ? tty : stdin, *out = tty ? tty : stderr;
    int echo_off, rc = PLAT_OK;

    fputs(prompt, out);
    fflush(out);
    echo_off = tcgetattr(fileno(in), &old) == 0;
    if (echo_off) {
        quiet = old;
        quiet.c_lflag &= ~(tcflag_t)ECHO;
        tcsetattr(fileno(in), TCSAFLUSH, &quiet);
    }
    if (!fgets(buf, (int)bufsize, in))
        rc = got_break ? PLAT_EBREAK : PLAT_EIO;
    else
        buf[strcspn(buf, "\r\n")] = '\0';
    if (echo_off)
        tcsetattr(fileno(in), TCSAFLUSH, &old);
    fputs("\n", out);
    if (tty)
        fclose(tty);
    return rc;
}

void plat_localtime(int64_t t, ars_tm *tm)
{
    time_t tt = (time_t)t;
    struct tm lt;

    if (!localtime_r(&tt, &lt)) {
        secs_to_civil(t, tm);
        return;
    }
    tm->year = lt.tm_year + 1900;
    tm->month = lt.tm_mon + 1;
    tm->day = lt.tm_mday;
    tm->hour = lt.tm_hour;
    tm->minute = lt.tm_min;
    tm->second = lt.tm_sec;
}

uint32_t plat_millis(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)((uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000);
}

int64_t plat_max_file_size(void)
{
    return INT64_MAX;
}

int plat_name_max(const char *dir)
{
    long n = pathconf(dir, _PC_NAME_MAX);

    return n > 0 ? (int)n : 255;
}

int plat_case_insensitive(void)
{
    return 0;
}

int64_t plat_clamp_mtime(int64_t t)
{
    return t;
}

/* --- sockets --- */

int plat_tcp_connect(const char *host, int port, int *sock_out)
{
    struct addrinfo hints, *res, *ai;
    char portstr[16];
    int s = -1, rc;

    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;   /* IPv4 only, matching the Amiga stacks */
    hints.ai_socktype = SOCK_STREAM;
    snprintf(portstr, sizeof portstr, "%d", port);
    if (getaddrinfo(host, portstr, &hints, &res) != 0)
        return PLAT_EHOST;

    rc = PLAT_ECONN;
    for (ai = res; ai; ai = ai->ai_next) {
        s = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (s < 0)
            continue;
        if (connect(s, ai->ai_addr, ai->ai_addrlen) == 0) {
            rc = PLAT_OK;
            break;
        }
        rc = errno == EINTR && got_break ? PLAT_EBREAK : PLAT_ECONN;
        close(s);
        s = -1;
        if (rc == PLAT_EBREAK)
            break;
    }
    freeaddrinfo(res);
    if (rc != PLAT_OK)
        return rc;

    if (fcntl(s, F_SETFL, fcntl(s, F_GETFL) | O_NONBLOCK) < 0) {
        close(s);
        return PLAT_EIO;
    }
    *sock_out = s;
    return PLAT_OK;
}

long plat_sock_send(int sock, const void *buf, size_t n)
{
    ssize_t r = send(sock, buf, n, MSG_NOSIGNAL);

    if (r >= 0)
        return (long)r;
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
        return PLAT_EAGAIN;
    return PLAT_EIO;
}

long plat_sock_recv(int sock, void *buf, size_t n)
{
    ssize_t r = recv(sock, buf, n, 0);

    if (r >= 0)
        return (long)r;
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
        return PLAT_EAGAIN;
    return PLAT_EIO;
}

int plat_sock_wait(int sock, int want_read, int want_write, long timeout_ms,
                   int *readable, int *writable)
{
    struct pollfd pfd;
    int r;

    *readable = *writable = 0;
    if (got_break)
        return PLAT_EBREAK;
    pfd.fd = sock;
    pfd.events = (short)((want_read ? POLLIN : 0) | (want_write ? POLLOUT : 0));
    pfd.revents = 0;
    r = poll(&pfd, 1, timeout_ms < 0 ? -1 : (int)timeout_ms);
    if (r < 0)
        return errno == EINTR ? (got_break ? PLAT_EBREAK : PLAT_ETIMEOUT) : PLAT_EIO;
    if (r == 0)
        return PLAT_ETIMEOUT;
    /* Errors and hangups count as readable so the next recv() reports them. */
    *readable = (pfd.revents & (POLLIN | POLLHUP | POLLERR)) != 0;
    *writable = (pfd.revents & POLLOUT) != 0;
    return PLAT_OK;
}

void plat_sock_close(int sock)
{
    close(sock);
}

/* --- files --- */

static int open_file(const char *path, int flags, plat_file **out)
{
    plat_file *f;
    int fd = open(path, flags, 0666);

    if (fd < 0)
        return map_errno(errno);
    if (!(f = malloc(sizeof *f))) {
        close(fd);
        return PLAT_ENOMEM;
    }
    f->fd = fd;
    *out = f;
    return PLAT_OK;
}

int plat_open_read(const char *path, plat_file **out)
{
    return open_file(path, O_RDONLY, out);
}

int plat_open_write(const char *path, plat_file **out)
{
    return open_file(path, O_WRONLY | O_CREAT | O_TRUNC, out);
}

long plat_read(plat_file *f, void *buf, size_t n)
{
    ssize_t r;

    do
        r = read(f->fd, buf, n);
    while (r < 0 && errno == EINTR && !got_break);
    return r < 0 ? map_errno(errno) : (long)r;
}

int plat_write(plat_file *f, const void *buf, size_t n)
{
    const char *p = buf;
    ssize_t r;

    while (n > 0) {
        r = write(f->fd, p, n);
        if (r < 0) {
            if (errno == EINTR && !got_break)
                continue;
            return map_errno(errno);
        }
        p += r;
        n -= (size_t)r;
    }
    return PLAT_OK;
}

int plat_seek(plat_file *f, int64_t offset)
{
    return lseek(f->fd, (off_t)offset, SEEK_SET) < 0 ? map_errno(errno) : PLAT_OK;
}

int plat_close(plat_file *f)
{
    int rc = close(f->fd) < 0 ? map_errno(errno) : PLAT_OK;

    free(f);
    return rc;
}

static void fill_info(const struct stat *sb, plat_fileinfo *st)
{
    st->type = S_ISREG(sb->st_mode) ? PLAT_T_FILE : S_ISDIR(sb->st_mode) ? PLAT_T_DIR
             : S_ISLNK(sb->st_mode) ? PLAT_T_LINK : PLAT_T_OTHER;
    st->size = (int64_t)sb->st_size;
    st->mtime = (int64_t)sb->st_mtim.tv_sec;
    st->mtime_nsec = sb->st_mtim.tv_nsec;
    st->perms = (uint32_t)(sb->st_mode & 07777);
}

int plat_stat(const char *path, plat_fileinfo *st)
{
    struct stat sb;

    if (stat(path, &sb) < 0)
        return map_errno(errno);
    fill_info(&sb, st);
    return PLAT_OK;
}

int plat_mkdir(const char *path)
{
    return mkdir(path, 0777) < 0 ? map_errno(errno) : PLAT_OK;
}

int plat_remove(const char *path)
{
    return remove(path) < 0 ? map_errno(errno) : PLAT_OK;
}

int plat_rename(const char *from, const char *to)
{
    return rename(from, to) < 0 ? map_errno(errno) : PLAT_OK;
}

int plat_set_mtime(const char *path, int64_t mtime, long nsec)
{
    struct timespec ts[2];

    ts[0].tv_sec = 0;
    ts[0].tv_nsec = UTIME_OMIT;    /* leave atime alone */
    ts[1].tv_sec = (time_t)mtime;
    ts[1].tv_nsec = nsec;
    return utimensat(AT_FDCWD, path, ts, 0) < 0 ? map_errno(errno) : PLAT_OK;
}

int plat_new_file_perms(const char *path, uint32_t wire_mode, const char *existing)
{
    struct stat sb;
    mode_t mode;

    if (existing && stat(existing, &sb) == 0)
        mode = sb.st_mode & 07777;
    else
        mode = (mode_t)(wire_mode & 0777) & ~saved_umask;
    return chmod(path, mode) < 0 ? map_errno(errno) : PLAT_OK;
}

int plat_opendir(const char *path, plat_dir **out)
{
    plat_dir *d;
    DIR *dir = opendir(path);

    if (!dir)
        return map_errno(errno);
    if (!(d = malloc(sizeof *d))) {
        closedir(dir);
        return PLAT_ENOMEM;
    }
    d->dir = dir;
    snprintf(d->path, sizeof d->path, "%s", path);
    *out = d;
    return PLAT_OK;
}

int plat_readdir(plat_dir *d, char *name, size_t namesize, plat_fileinfo *info)
{
    struct dirent *de;
    struct stat sb;
    char full[4096 + 256];

    for (;;) {
        errno = 0;
        if (!(de = readdir(d->dir)))
            return errno ? map_errno(errno) : 0;
        if (strcmp(de->d_name, ".") != 0 && strcmp(de->d_name, "..") != 0)
            break;
    }
    if (strlen(de->d_name) >= namesize)
        return PLAT_ETOOBIG;
    strcpy(name, de->d_name);
    if (info) {
        snprintf(full, sizeof full, "%s/%s", d->path, de->d_name);
        if (lstat(full, &sb) < 0)
            return map_errno(errno);
        fill_info(&sb, info);
    }
    return 1;
}

void plat_closedir(plat_dir *d)
{
    closedir(d->dir);
    free(d);
}
