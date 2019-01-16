// This helper library is LD_PRELOAD-ed by out+err into a COMMAND
// it launches. The library
//
// * switches stdout to line-buffered mode (fully buffered by
//   default on non-tty device);
//
// * binary-patches write() and writev() to retry calls with a smaller
//   data chunk if failed with EMSGSIZE. The failure happens when
//   write() is called with a UNIX dgram socket used for stdin/stderr.
#define _GNU_SOURCE 1
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <unistd.h>

#include "hook_engine/hook_engine.h"

static unsigned send_buf_size;

ssize_t __real__write(int fd, const void *buf, size_t count);
HOOK_DEFINE_TRAMPOLINE(__real__write);

static ssize_t __wrap__write(int fd, const void *buf, size_t count) {
    ssize_t rc = __real__write(fd, buf, count);
    if (rc == -1 && errno == EMSGSIZE && count > send_buf_size/2) {
        return __real__write(fd, buf, send_buf_size/2);
    }
    return rc;
}

ssize_t __real__writev(int fd, const struct iovec *iov, int iovcnt);
HOOK_DEFINE_TRAMPOLINE(__real__writev);

static ssize_t __wrap__writev(int fd, const struct iovec *iov, int iovcnt) {
    ssize_t rc = __real__writev(fd, iov, iovcnt);
    if (rc == -1 && errno == EMSGSIZE && iovcnt <= IOV_MAX) {
        struct iovec iov_copy[IOV_MAX];
        int n = 0;
        size_t size = 0;
        for (; n < iovcnt; ++n) {
            iov_copy[n] = iov[n];
            size += iov[n].iov_len;
            if (size >= send_buf_size/2) {
                iov_copy[n++].iov_len -= size - send_buf_size/2;
                break;
            }
        }
        if (size > send_buf_size/2) {
            return __real__writev(fd, iov_copy, n);
        }
    }
    return rc;
}

#ifdef MUSL
// MUSL uses bare syscalls in its stdio implementation, forcing us to
// replace a whole function.
// https://github.com/ifduyue/musl/blob/79f653c6bc2881dd6855299c908a442f56cb7c2b/src/internal/stdio_impl.h#L21
struct _MUSL_FILE {
    unsigned flags;
    unsigned char *rpos, *rend;
    int (*close)(FILE *);
    unsigned char *wend, *wpos;
    unsigned char *mustbezero_1;
    unsigned char *wbase;
    size_t (*read)(struct _MUSL_FILE *, unsigned char *, size_t);
    size_t (*write)(struct _MUSL_FILE *, const unsigned char *, size_t);
    off_t (*seek)(struct _MUSL_FILE *, off_t, int);
    unsigned char *buf;
    size_t buf_size;
    struct _MUSL_FILE *prev, *next;
    int fd;
    // more irrelevant members follow
};

#define F_ERR 32

size_t __stdio_write(
    struct _MUSL_FILE *f, const unsigned char *buf, size_t len
);

// https://github.com/ifduyue/musl/blob/b4b1e10364c8737a632be61582e05a8d3acf5690/src/stdio/__stdio_write.c#L4
static size_t __wrap__stdio_write(
    struct _MUSL_FILE *f, const unsigned char *buf, size_t len
) {
    struct iovec iovs[2] = {
        { .iov_base = f->wbase, .iov_len = f->wpos-f->wbase },
        { .iov_base = (void *)buf, .iov_len = len }
    };
    struct iovec *iov = iovs;
    size_t rem = iov[0].iov_len + iov[1].iov_len;
    int iovcnt = 2;
    ssize_t cnt;
    for (;;) {
        cnt = __wrap__writev(f->fd, iov, iovcnt);
        if (cnt == rem) {
            f->wend = f->buf + f->buf_size;
            f->wpos = f->wbase = f->buf;
            return len;
        }
        if (cnt < 0) {
            f->wpos = f->wbase = f->wend = 0;
            f->flags |= F_ERR;
            return iovcnt == 2 ? 0 : len-iov[0].iov_len;
        }
        rem -= cnt;
        if (cnt > iov[0].iov_len) {
            cnt -= iov[0].iov_len;
            iov++; iovcnt--;
        }
        iov[0].iov_base = (char *)iov[0].iov_base + cnt;
        iov[0].iov_len -= cnt;
    }
}
#endif

void init(void) {
    int sock;
    socklen_t len = sizeof send_buf_size;
    setvbuf(stdout, NULL, _IOLBF, 0);
    sock = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (
        sock == -1 ||
        getsockopt(sock, SOL_SOCKET, SO_SNDBUF, &send_buf_size, &len) != 0
    ) {
        fprintf(
            stderr, "%s: Get socket send buffer size: %s\n",
            program_invocation_name, strerror(errno)
        );
        send_buf_size = 0x8000;
    }
    if (sock != -1) close(sock);
    if (
        hook_begin() != 0 ||
        hook_install(write,  __wrap__write,  __real__write ) != 0 ||
        hook_install(writev, __wrap__writev, __real__writev) != 0
#ifdef MUSL
        || hook_install(__stdio_write, __wrap__stdio_write, NULL) != 0
#endif
    ) {
        fprintf(
            stderr, "%s: %s\n", program_invocation_name, hook_last_error()
        );
        exit(EXIT_FAILURE);
    }
    hook_end();
}
