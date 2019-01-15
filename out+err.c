// Usage: out+err [-o FILE] COMMAND [ARG]...
//
// Run COMMAND, combining chunks sent to STDOUT and STDERR into a single
// file, preserving the relative order.  Every chunk starts with a
// 4 byte header.  A header encodes the data size as a 32 bit big endian
// number.  Only 31 lower bits are used.  The high bit is 0 for STDOUT,
// 1 for STDERR.
#define _GNU_SOURCE 1
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <unistd.h>

static int master_sock;
static volatile int child_status;

static void sigchld_handler(int sig) {
    const int errno_old = errno;
    int status;
    if (
        waitpid(-1, &status, WNOHANG) > 0 ||
        (WIFEXITED(status) || WIFSIGNALED(status))
    ) {
        child_status = status;
        fcntl(master_sock, F_SETFL, O_NONBLOCK);
    }
    errno = errno_old;
}

static void usage(void) {
    fprintf(
        stderr, "Usage: %s [-o FILE] COMMAND [ARG]...\n",
        program_invocation_name
    );
    exit(EXIT_FAILURE);
}

static void fail(const char *msg) __attribute__((noreturn));
static void fail(const char *msg) {
    fprintf(
        stderr, "%s: %s: %s\n",
        program_invocation_name, msg, strerror(errno)
    );
    exit(EXIT_FAILURE);
}

static int make_socket(struct sockaddr_un* addr, socklen_t *addrlen) {
    int sock;
    if ((sock = socket(AF_UNIX, SOCK_DGRAM, 0)) == -1) fail("socket");
    addr->sun_family = AF_UNIX;
    if (bind(
            sock, (struct sockaddr*)addr,
            offsetof(struct sockaddr_un, sun_path)
        ) != 0
    ) {
        fail("bind");
    }
    *addrlen = sizeof(*addr);
    if (getsockname(sock, (struct sockaddr*)addr, addrlen) != 0) {
        fail("getsockname");
    }
    return sock;
}

static size_t wmem_max(void) {
#define WMEM_MAX "/proc/sys/net/core/wmem_max"
    FILE *f;
    size_t sz;
    f = fopen(WMEM_MAX, "r");
    if (!f || fscanf(f, "%zu", &sz) != 1) fail(WMEM_MAX);
    fclose(f);
    return sz;
}

int main(int argc, char **argv) {

    int opt, fd;
    int output_sock, error_sock;
    struct sockaddr_un master_addr, output_addr, error_addr;
    socklen_t master_addrlen, output_addrlen, error_addrlen;
    size_t msg_size_max;
    void *msg_buf;
    struct sockaddr_un msg_addr;
    socklen_t msg_addrlen;
    ssize_t rc;
    int status;

    while ((opt = getopt(argc, argv, "+o:")) != -1) {
        switch (opt) {
        case 'o':
            fd = open(
                optarg, O_CREAT | O_TRUNC | O_WRONLY | O_CLOEXEC, 0600
            );
            if (fd == -1 || dup3(fd, STDOUT_FILENO, 0) != STDOUT_FILENO) {
                fprintf(
                    stderr, "%s: %s: %s\n",
                    program_invocation_name, optarg, strerror(errno)
                );
                exit(EXIT_FAILURE);
            }
            break;
        default:
            usage();
        }
    }
    if (optind >= argc) usage();

    master_sock = make_socket(&master_addr, &master_addrlen);
    output_sock = make_socket(&output_addr, &output_addrlen);
    error_sock  = make_socket(&error_addr,  &error_addrlen);

    msg_size_max = wmem_max();
    if (!(msg_buf = malloc(msg_size_max))) fail("malloc");

    if (
        connect(
            output_sock, (struct sockaddr *)&master_addr, master_addrlen
        ) != 0 ||
        connect(
            error_sock, (struct sockaddr *)&master_addr, master_addrlen
        ) != 0
    ) {
        fail("connect");
    }

    if (signal(SIGCHLD, sigchld_handler) != 0) fail("signal");

    switch (fork()) {
    case -1:
        fail("fork");
    case 0:
        if (
            close(master_sock) != 0 ||
            dup3(output_sock, STDOUT_FILENO, 0) != STDOUT_FILENO ||
            close(output_sock) != 0 ||
            dup3(error_sock, STDERR_FILENO, 0) != STDERR_FILENO ||
            close(error_sock) != 0
        ) {
            fail("Redirect stdout/stderr");
        }
#ifdef HELPER_SO
        if (putenv("LD_PRELOAD="HELPER_SO) != 0) fail("putenv");
#endif
        execvp(argv[optind], argv + optind);
        fprintf(
            stderr, "%s: Failed to run '%s': %s\n",
            program_invocation_name, argv[optind], strerror(errno)
        );
        return EXIT_FAILURE;
    }

    while (1) {
        uint32_t header;
        struct iovec iov[2];
        msg_addrlen = sizeof msg_addr;
        rc = recvfrom(
            master_sock, msg_buf, msg_size_max, 0, &msg_addr, &msg_addrlen
        );
        if (rc < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            if (errno == EINTR) continue;
            fail("recvfrom");
        }
        if (
            msg_addrlen == output_addrlen &&
            !memcmp(&msg_addr, &output_addr, output_addrlen)
        ) {
            header = htonl(rc);
        } else if (
            msg_addrlen == error_addrlen &&
            !memcmp(&msg_addr, &error_addr, error_addrlen)
        ) {
            header = htonl(UINT32_C(0x80000000) | rc);
        } else {
            continue;
        }
        iov[0].iov_base = &header;
        iov[0].iov_len = 4;
        iov[1].iov_base = msg_buf;
        iov[1].iov_len = rc;
        while (iov[0].iov_len) {
            rc = writev(STDOUT_FILENO, iov, 2);
            if (rc < 0) {
                if (errno == EINTR) continue;
                fail("writev");
            }
            while (rc && iov[0].iov_len <= rc) {
                rc -= iov[0].iov_len;
                iov[0] = iov[1];
                iov[1].iov_len = 0;
            }
            iov[0].iov_base += rc;
            iov[0].iov_len -= rc;
        }
    }

    status = child_status;
    if (WIFSIGNALED(status)) {
        kill(getpid(), WTERMSIG(status));
    }
    return WEXITSTATUS(status);
}
