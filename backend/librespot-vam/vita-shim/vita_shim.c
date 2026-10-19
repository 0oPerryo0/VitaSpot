/*
 * vita_shim.c - small compatibility layer needed when linking the Rust
 * backend into the VitaSpot VPK.
 *
 * Rust's std (newlib target) and the `rustix` crate reference a few POSIX
 * functions that the VitaSDK newlib does not provide. None of them are used by
 * VitaSpot at runtime (the code paths are only reachable for process spawning,
 * symlink following and vectored stdio), but the static archive references
 * them, so the linker needs definitions.
 *
 * The exception is the pthread_cond_timedwait() wrapper at the end of this
 * file, which runs on every timed Condvar wait.
 *
 * Build with:
 *   arm-vita-eabi-gcc -O2 -c vita_shim.c -o vita_shim.o
 *   arm-vita-eabi-ar rcs libvam_vita_shim.a vita_shim.o
 */

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

/* VitaSDK newlib has no <sys/uio.h>; define the POSIX iovec here. */
struct iovec {
    void *iov_base;
    size_t iov_len;
};

#ifndef ENOSYS
#define ENOSYS 38
#endif
#ifndef ENOTTY
#define ENOTTY 25
#endif

/* std::os::unix::process::parent_id() */
pid_t getppid(void) { return 1; }

/* std::fs::read_link() / rustix::fs::readlinkat() */
ssize_t readlinkat(int dirfd, const char *path, char *buf, size_t bufsiz)
{
    (void)dirfd;
    (void)path;
    (void)buf;
    (void)bufsiz;
    errno = ENOSYS;
    return -1;
}

/* rustix::io::dup2() */
int dup2(int oldfd, int newfd)
{
    (void)oldfd;
    (void)newfd;
    errno = ENOSYS;
    return -1;
}

/* rustix::io::readv() */
ssize_t readv(int fd, const struct iovec *iov, int iovcnt)
{
    ssize_t total = 0;
    for (int i = 0; i < iovcnt; i++) {
        ssize_t got = read(fd, iov[i].iov_base, iov[i].iov_len);
        if (got < 0)
            return total > 0 ? total : got;
        total += got;
        if ((size_t)got < iov[i].iov_len)
            break;
    }
    return total;
}

/* rustix::io::writev() */
ssize_t writev(int fd, const struct iovec *iov, int iovcnt)
{
    ssize_t total = 0;
    for (int i = 0; i < iovcnt; i++) {
        ssize_t put = write(fd, iov[i].iov_base, iov[i].iov_len);
        if (put < 0)
            return total > 0 ? total : put;
        total += put;
        if ((size_t)put < iov[i].iov_len)
            break;
    }
    return total;
}

/* rustix::ioctl - not used by VitaSpot's code paths. */
int ioctl(int fd, unsigned long request, ...)
{
    (void)fd;
    (void)request;
    errno = ENOTTY;
    return -1;
}

/* rustix::fs::readlink */
ssize_t readlink(const char *path, char *buf, size_t bufsiz)
{
    (void)path;
    (void)buf;
    (void)bufsiz;
    errno = ENOSYS;
    return -1;
}

/* rustix::fs::fchown */
int fchown(int fd, uid_t owner, gid_t group)
{
    (void)fd;
    (void)owner;
    (void)group;
    return 0;
}

/* std::os::unix::fs::lchown */
int lchown(const char *path, uid_t owner, gid_t group)
{
    (void)path;
    (void)owner;
    (void)group;
    return 0;
}

/* std::os::unix::fs::mkfifo (not supported by the Vita filesystem) */
int mkfifo(const char *path, mode_t mode)
{
    (void)path;
    (void)mode;
    errno = ENOSYS;
    return -1;
}

/* The *at() family: newlib has no directory-relative syscalls. Ignore the
 * directory fd and forward to the plain path based call where one exists. */
int openat(int dirfd, const char *path, int flags, ...)
{
    (void)dirfd;
    mode_t mode = 0;
    if (flags & O_CREAT) {
        va_list ap;
        va_start(ap, flags);
        mode = (mode_t)va_arg(ap, int);
        va_end(ap);
    }
    return open(path, flags, mode);
}

int fstatat(int dirfd, const char *path, struct stat *st, int flags)
{
    (void)dirfd;
    (void)flags;
    return stat(path, st);
}

int fchmodat(int dirfd, const char *path, mode_t mode, int flags)
{
    (void)dirfd;
    (void)flags;
    return chmod(path, mode);
}

int mkdirat(int dirfd, const char *path, mode_t mode)
{
    (void)dirfd;
    return mkdir(path, mode);
}

int renameat(int olddirfd, const char *oldpath, int newdirfd, const char *newpath)
{
    (void)olddirfd;
    (void)newdirfd;
    return rename(oldpath, newpath);
}

int unlinkat(int dirfd, const char *path, int flags)
{
    (void)dirfd;
    (void)flags;
    return unlink(path);
}

int linkat(int olddirfd, const char *oldpath, int newdirfd, const char *newpath, int flags)
{
    (void)olddirfd;
    (void)oldpath;
    (void)newdirfd;
    (void)newpath;
    (void)flags;
    errno = ENOSYS;
    return -1;
}

int symlink(const char *target, const char *linkpath)
{
    (void)target;
    (void)linkpath;
    errno = ENOSYS;
    return -1;
}

int symlinkat(const char *target, int newdirfd, const char *linkpath)
{
    (void)target;
    (void)newdirfd;
    (void)linkpath;
    errno = ENOSYS;
    return -1;
}

/*
 * pthread_cond_timedwait() deadline fix (linked with
 * -Wl,--wrap=pthread_cond_timedwait, see scripts/build-vpk.sh).
 *
 * Rust's std calls pthread_condattr_setclock(CLOCK_MONOTONIC) and passes
 * deadlines measured on CLOCK_MONOTONIC (time since boot). VitaSDK's
 * pthread-embedded accepts that call but ignores the clock: the wait compares
 * the deadline against the wall clock (ftime()). A monotonic deadline is then
 * decades in the past, so every Condvar::wait_timeout() returned "timed out"
 * at once. librespot's audio download wait failed that way, and every track
 * was skipped as unavailable.
 *
 * A wall-clock deadline is always after 2000-01-01; anything earlier is a
 * monotonic deadline and is moved onto the wall clock here.
 */
#include <pthread.h>
#include <time.h>

#define VAM_WALL_CLOCK_MIN_SEC 946684800LL /* 2000-01-01T00:00:00Z */
#define VAM_NSEC_PER_SEC 1000000000LL

int __real_pthread_cond_timedwait(pthread_cond_t *cond, pthread_mutex_t *mutex,
                                  const struct timespec *abstime);

int __wrap_pthread_cond_timedwait(pthread_cond_t *cond, pthread_mutex_t *mutex,
                                  const struct timespec *abstime)
{
    struct timespec mono, wall, fixed;
    int64_t remaining_ns, wall_ns;

    if (abstime == NULL || (int64_t)abstime->tv_sec >= VAM_WALL_CLOCK_MIN_SEC)
        return __real_pthread_cond_timedwait(cond, mutex, abstime);
    if (clock_gettime(CLOCK_MONOTONIC, &mono) != 0 ||
        clock_gettime(CLOCK_REALTIME, &wall) != 0)
        return __real_pthread_cond_timedwait(cond, mutex, abstime);

    remaining_ns = ((int64_t)abstime->tv_sec - (int64_t)mono.tv_sec) * VAM_NSEC_PER_SEC +
                   ((int64_t)abstime->tv_nsec - (int64_t)mono.tv_nsec);
    if (remaining_ns < 0)
        remaining_ns = 0;

    wall_ns = (int64_t)wall.tv_nsec + remaining_ns;
    fixed.tv_sec = wall.tv_sec + (time_t)(wall_ns / VAM_NSEC_PER_SEC);
    fixed.tv_nsec = (long)(wall_ns % VAM_NSEC_PER_SEC);
    return __real_pthread_cond_timedwait(cond, mutex, &fixed);
}
