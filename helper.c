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
    ) {
        fprintf(
            stderr, "%s: %s\n", program_invocation_name, hook_last_error()
        );
    }
    hook_end();
}
