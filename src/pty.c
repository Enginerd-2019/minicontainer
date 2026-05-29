// Note: _GNU_SOURCE is provided by the Makefile.
#include "pty.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <errno.h>

int pty_open_in_child(pty_pair_t *pair, bool enable_debug) {
    pair->master_fd = -1;
    pair->slave_fd  = -1;
    pair->slave_path[0] = '\0';

    /* /dev/ptmx is bound to /dev/pts/ptmx by mount_devpts; this opens
     * the container's newinstance master multiplexer. */
    pair->master_fd = posix_openpt(O_RDWR | O_NOCTTY);
    if (pair->master_fd < 0) {
        perror("posix_openpt");
        return -1;
    }
    if (grantpt(pair->master_fd) < 0) {
        perror("grantpt");
        close(pair->master_fd);
        pair->master_fd = -1;
        return -1;
    }
    if (unlockpt(pair->master_fd) < 0) {
        perror("unlockpt");
        close(pair->master_fd);
        pair->master_fd = -1;
        return -1;
    }

    const char *slave = ptsname(pair->master_fd);
    if (!slave) {
        perror("ptsname");
        close(pair->master_fd);
        pair->master_fd = -1;
        return -1;
    }
    strncpy(pair->slave_path, slave, sizeof(pair->slave_path) - 1);
    pair->slave_path[sizeof(pair->slave_path) - 1] = '\0';

    /* Open the slave inside the container.  Path is /dev/pts/<N>
     * inside the container's newinstance devpts, fully valid here. */
    pair->slave_fd = open(pair->slave_path, O_RDWR);
    if (pair->slave_fd < 0) {
        fprintf(stderr, "[pty] open(%s): %s\n",
                pair->slave_path, strerror(errno));
        close(pair->master_fd);
        pair->master_fd = -1;
        return -1;
    }

    if (enable_debug) {
        fprintf(stderr, "[pty] child opened master fd %d, slave %s (fd %d)\n",
                pair->master_fd, pair->slave_path, pair->slave_fd);
    }
    return 0;
}

int pty_send_master(int sock_fd, int fd) {
    /* Standard SCM_RIGHTS sender boilerplate.  Must send at least one
     * byte of regular payload — the kernel will not deliver the cmsg
     * on a zero-byte iov. */
    char buf[1] = { 0 };
    struct iovec iov = { .iov_base = buf, .iov_len = 1 };

    union {
        char buf[CMSG_SPACE(sizeof(int))];
        struct cmsghdr align;
    } cmsg_buf = {0};

    struct msghdr msg = {
        .msg_iov        = &iov,
        .msg_iovlen     = 1,
        .msg_control    = cmsg_buf.buf,
        .msg_controllen = sizeof(cmsg_buf.buf),
    };

    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type  = SCM_RIGHTS;
    cmsg->cmsg_len   = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(cmsg), &fd, sizeof(int));

    ssize_t n = sendmsg(sock_fd, &msg, 0);
    if (n < 0) {
        perror("sendmsg(SCM_RIGHTS)");
        return -1;
    }
    return 0;
}

int pty_recv_master(int sock_fd, int *out_fd) {
    char buf[1] = { 0 };
    struct iovec iov = { .iov_base = buf, .iov_len = 1 };

    union {
        char buf[CMSG_SPACE(sizeof(int))];
        struct cmsghdr align;
    } cmsg_buf = {0};

    struct msghdr msg = {
        .msg_iov        = &iov,
        .msg_iovlen     = 1,
        .msg_control    = cmsg_buf.buf,
        .msg_controllen = sizeof(cmsg_buf.buf),
    };

    ssize_t n = recvmsg(sock_fd, &msg, 0);
    if (n < 0) {
        perror("recvmsg(SCM_RIGHTS)");
        return -1;
    }
    if (n == 0) {
        fprintf(stderr, "[pty] peer closed before sending master\n");
        return -1;
    }

    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
    if (!cmsg ||
        cmsg->cmsg_level != SOL_SOCKET ||
        cmsg->cmsg_type  != SCM_RIGHTS ||
        cmsg->cmsg_len   != CMSG_LEN(sizeof(int))) {
        fprintf(stderr, "[pty] recvmsg returned no SCM_RIGHTS cmsg\n");
        return -1;
    }

    int fd;
    memcpy(&fd, CMSG_DATA(cmsg), sizeof(int));
    *out_fd = fd;
    return 0;
}

int pty_set_ctty(int slave_fd) {
    if (slave_fd < 0) return -1;

    /* Become session leader and make the slave our controlling
     * terminal.  setsid() must precede TIOCSCTTY because TIOCSCTTY
     * requires the caller to be a session leader without an existing
     * controlling terminal. */
    if (setsid() < 0) { perror("setsid"); return -1; }
    if (ioctl(slave_fd, TIOCSCTTY, 0) < 0) {
        perror("ioctl(TIOCSCTTY)");
        return -1;
    }

    /* dup over stdin/stdout/stderr.  After this the original slave_fd
     * is redundant; close it so close_inherited_fds doesn't have to. */
    if (dup2(slave_fd, STDIN_FILENO)  < 0) { perror("dup2 stdin");  return -1; }
    if (dup2(slave_fd, STDOUT_FILENO) < 0) { perror("dup2 stdout"); return -1; }
    if (dup2(slave_fd, STDERR_FILENO) < 0) { perror("dup2 stderr"); return -1; }
    if (slave_fd > STDERR_FILENO) close(slave_fd);
    return 0;
}

int pty_forward(int master_fd) {
    /* Forward bytes between user's terminal (stdin/stdout) and
     * master_fd. Loop on select() until master_fd EOFs. */
    struct termios old_tio, new_tio;
    tcgetattr(STDIN_FILENO, &old_tio);
    new_tio = old_tio;
    cfmakeraw(&new_tio);
    tcsetattr(STDIN_FILENO, TCSANOW, &new_tio);

    char buf[4096];
    for (;;) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(STDIN_FILENO, &fds);
        FD_SET(master_fd, &fds);
        int max = master_fd > STDIN_FILENO ? master_fd : STDIN_FILENO;
        if (select(max + 1, &fds, NULL, NULL, NULL) < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (FD_ISSET(master_fd, &fds)) {
            ssize_t n = read(master_fd, buf, sizeof(buf));
            if (n <= 0) break;
            write(STDOUT_FILENO, buf, n);
        }
        if (FD_ISSET(STDIN_FILENO, &fds)) {
            ssize_t n = read(STDIN_FILENO, buf, sizeof(buf));
            if (n <= 0) break;
            write(master_fd, buf, n);
        }
    }

    tcsetattr(STDIN_FILENO, TCSANOW, &old_tio);
    return 0;
}