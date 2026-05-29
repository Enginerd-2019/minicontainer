#ifndef PTY_H
#define PTY_H

#include <stdbool.h>

typedef struct {
    int  master_fd;        // Master end. Child sends to parent via SCM_RIGHTS.
    int  slave_fd;         // Slave end. Child only.
    char slave_path[64];   // /dev/pts/<N> inside container; child only.
} pty_pair_t;

/**
 * Inside the child, AFTER mount_devpts: open master via
 * posix_openpt(O_RDWR | O_NOCTTY), then grantpt/unlockpt/ptsname,
 * then open the slave by path. On success, pair->master_fd and
 * pair->slave_fd are both set, pair->slave_path is filled.
 *
 * MUST run after mount_devpts so /dev/ptmx points at the
 * container's newinstance master multiplexer.
 *
 * @param pair          Output: master_fd, slave_fd, slave_path
 * @param enable_debug  Verbose
 * @return  0 on success, -1 on failure
 */
int pty_open_in_child(pty_pair_t *pair, bool enable_debug);

/**
 * Send an open fd to the peer over a SOCK_STREAM Unix-domain
 * socket via SCM_RIGHTS. The kernel duplicates the underlying open
 * file description into the receiver's fd table; the sender may
 * close its copy after sendmsg returns.
 *
 * @param sock_fd  Socketpair endpoint (child's sv[1])
 * @param fd       Open fd to send (the master)
 * @return  0 on success, -1 on failure
 */
int pty_send_master(int sock_fd, int fd);

/**
 * Receive a single fd over a SOCK_STREAM Unix-domain socket via
 * SCM_RIGHTS. On success, *out_fd is the receiver-side fd
 * referencing the same open file description the peer sent.
 *
 * @param sock_fd  Socketpair endpoint (parent's sv[0])
 * @param out_fd   Output: received fd
 * @return  0 on success, -1 on failure
 */
int pty_recv_master(int sock_fd, int *out_fd);

/**
 * Inside child after pty_open_in_child + pty_send_master:
 * setsid(), ioctl(TIOCSCTTY), dup the slave fd over
 * stdin/stdout/stderr, close the original slave fd.
 *
 * @param slave_fd  pair->slave_fd from pty_open_in_child
 * @return  0 on success, -1 on failure
 */
int pty_set_ctty(int slave_fd);

/**
 * Forward bytes between PTY master and the parent's terminal.
 * Blocks until the master EOFs (child exits).
 */
int pty_forward(int master_fd);

#endif // PTY_H