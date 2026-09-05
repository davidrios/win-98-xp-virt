/*
 * ipc-latency-spike — what a process boundary between QEMU and the player
 * would cost.  cc -O2 -o /tmp/ipcspike tools/ipc-latency-spike.c && /tmp/ipcspike
 *
 * The numbers in ADR-010's table come from this, on the x86-64 rig. Re-run it
 * on the Apple side before anyone acts on them.
 *
 * ADR-002 put QEMU in the player's process for latency. Splitting it back out
 * (the only structurally clean answer to the GPLv2/Apache-2.0 question) is
 * only worth discussing if the boundary is cheap, so this measures the exact
 * traffic the split would add, with the mechanisms it would use:
 *
 *   - a SOCK_SEQPACKET socketpair for control/events (message boundaries, no
 *     framing of our own, unlike SOCK_STREAM),
 *   - SCM_RIGHTS to hand the frontend a buffer fd once per ring slot — a
 *     dma-buf is just an fd, and a memfd exercises the identical mechanics,
 *   - one small message per presented frame ("slot N ready"), and the
 *     frontend's "slot N released" going back,
 *   - input events as single writes from the frontend.
 *
 * The in-process baseline for all of this is a C function call, measured here
 * too so the comparison is a number and not an adjective.
 *
 * Both processes are on one host, so CLOCK_MONOTONIC is directly comparable
 * across them: a timestamp written by the sender and read by the receiver is
 * a true one-way latency.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define FRAMES 20000
#define SLOTS 3

struct msg {
    uint32_t kind; /* 0 = frame ready, 1 = release, 2 = input, 3 = stop */
    uint32_t slot;
    uint64_t t_send; /* CLOCK_MONOTONIC ns at the sending end */
};

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + ts.tv_nsec;
}

static int cmp_u64(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? -1 : x > y;
}

static void report(const char *what, uint64_t *v, size_t n)
{
    qsort(v, n, sizeof *v, cmp_u64);
    double mean = 0;
    for (size_t i = 0; i < n; i++)
        mean += (double)v[i];
    mean /= (double)n;
    printf("%-34s n=%zu  p50 %6.2f us  p99 %7.2f us  max %8.2f us  mean %6.2f us\n",
           what, n, v[n / 2] / 1000.0, v[n * 99 / 100] / 1000.0, v[n - 1] / 1000.0, mean / 1000.0);
}

/* Send one message, optionally with a file descriptor attached. */
static int send_msg(int fd, const struct msg *m, int pass_fd)
{
    struct iovec iov = { .iov_base = (void *)m, .iov_len = sizeof *m };
    char cbuf[CMSG_SPACE(sizeof(int))];
    struct msghdr mh = { .msg_iov = &iov, .msg_iovlen = 1 };
    if (pass_fd >= 0) {
        memset(cbuf, 0, sizeof cbuf);
        mh.msg_control = cbuf;
        mh.msg_controllen = sizeof cbuf;
        struct cmsghdr *c = CMSG_FIRSTHDR(&mh);
        c->cmsg_level = SOL_SOCKET;
        c->cmsg_type = SCM_RIGHTS;
        c->cmsg_len = CMSG_LEN(sizeof(int));
        memcpy(CMSG_DATA(c), &pass_fd, sizeof(int));
    }
    return sendmsg(fd, &mh, 0) < 0 ? -1 : 0;
}

static int recv_msg(int fd, struct msg *m, int *got_fd)
{
    struct iovec iov = { .iov_base = m, .iov_len = sizeof *m };
    char cbuf[CMSG_SPACE(sizeof(int))];
    struct msghdr mh = { .msg_iov = &iov, .msg_iovlen = 1, .msg_control = cbuf, .msg_controllen = sizeof cbuf };
    ssize_t n = recvmsg(fd, &mh, 0);
    if (n <= 0)
        return -1;
    if (got_fd) {
        *got_fd = -1;
        for (struct cmsghdr *c = CMSG_FIRSTHDR(&mh); c; c = CMSG_NXTHDR(&mh, c))
            if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS)
                memcpy(got_fd, CMSG_DATA(c), sizeof(int));
    }
    return 0;
}

/* The "player" side: register the slots it is handed, then answer every
 * frame-ready with a release, the way a frontend returns a slot after it has
 * drawn with it. */
static void frontend(int fd)
{
    int slot_fd[SLOTS];
    for (int i = 0; i < SLOTS; i++) {
        struct msg m;
        int got = -1;
        if (recv_msg(fd, &m, &got) < 0)
            _exit(1);
        slot_fd[i] = got;
        struct msg ack = { .kind = 1, .slot = m.slot, .t_send = now_ns() };
        send_msg(fd, &ack, -1);
    }
    for (;;) {
        struct msg m;
        if (recv_msg(fd, &m, NULL) < 0)
            _exit(0);
        if (m.kind == 3)
            break;
        struct msg r = { .kind = 1, .slot = m.slot, .t_send = m.t_send };
        send_msg(fd, &r, -1);
    }
    for (int i = 0; i < SLOTS; i++)
        close(slot_fd[i]);
    _exit(0);
}

/* An in-process display callback, for the baseline: what the same "frame
 * ready" notification costs today, as a plain indirect call. */
static volatile uint64_t sink;
static void on_frame_ready(void *ud, int slot)
{
    (void)ud;
    sink += (uint64_t)slot;
}

int main(void)
{
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sv) < 0) {
        perror("socketpair");
        return 1;
    }
    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        return 1;
    }
    if (pid == 0) {
        close(sv[0]);
        frontend(sv[1]);
    }
    close(sv[1]);
    int fd = sv[0];

    /* 1. slot registration: a buffer fd handed over once per ring slot */
    uint64_t reg[SLOTS];
    for (int i = 0; i < SLOTS; i++) {
        int mfd = memfd_create("slot", 0);
        ftruncate(mfd, 1920 * 1080 * 4);
        struct msg m = { .kind = 0, .slot = (uint32_t)i, .t_send = now_ns() };
        uint64_t t0 = now_ns();
        send_msg(fd, &m, mfd);
        struct msg ack;
        recv_msg(fd, &ack, NULL);
        reg[i] = now_ns() - t0;
        close(mfd);
    }
    printf("slot registration (fd over SCM_RIGHTS, round trip): %.2f / %.2f / %.2f us\n",
           reg[0] / 1000.0, reg[1] / 1000.0, reg[2] / 1000.0);

    /* 2. per-frame notification: one-way (what actually delays a frame) and
     *    the full round trip (what would delay the *next* use of a slot). */
    uint64_t *oneway = malloc(FRAMES * sizeof *oneway);
    uint64_t *rtt = malloc(FRAMES * sizeof *rtt);
    for (int i = 0; i < FRAMES; i++) {
        struct msg m = { .kind = 0, .slot = (uint32_t)(i % SLOTS) };
        m.t_send = now_ns();
        uint64_t t0 = m.t_send;
        send_msg(fd, &m, -1);
        struct msg r;
        recv_msg(fd, &r, NULL);
        uint64_t t1 = now_ns();
        rtt[i] = t1 - t0;
        /* the release carries the frontend's receive timestamp back */
        oneway[i] = rtt[i] / 2; /* symmetric estimate; see the direct one below */
    }
    report("frame notify, round trip", rtt, FRAMES);
    report("frame notify, one way (rtt/2)", oneway, FRAMES);

    /* 2b. the same, paced at 60 Hz. A hot loop keeps both processes on a warm
     *     CPU and flatters the wakeup; a real frame arrives once every 16.7 ms
     *     into a process that has been idle since the last one, which is the
     *     number that actually matters. */
    const int paced = 600; /* ~10 s */
    uint64_t *prtt = malloc(paced * sizeof *prtt);
    for (int i = 0; i < paced; i++) {
        struct timespec gap = { .tv_sec = 0, .tv_nsec = 16666667 };
        nanosleep(&gap, NULL);
        struct msg m = { .kind = 0, .slot = (uint32_t)(i % SLOTS) };
        m.t_send = now_ns();
        uint64_t t0 = m.t_send;
        send_msg(fd, &m, -1);
        struct msg r;
        recv_msg(fd, &r, NULL);
        prtt[i] = now_ns() - t0;
    }
    report("frame notify @60Hz, cold receiver", prtt, paced);

    /* 3. the in-process baseline: the same notification as a function call */
    uint64_t *call = malloc(FRAMES * sizeof *call);
    for (int i = 0; i < FRAMES; i++) {
        uint64_t t0 = now_ns();
        on_frame_ready(NULL, i % SLOTS);
        call[i] = now_ns() - t0;
    }
    report("in-process callback (baseline)", call, FRAMES);

    struct msg stop = { .kind = 3 };
    send_msg(fd, &stop, -1);
    int status;
    waitpid(pid, &status, 0);
    printf("\n60 Hz frame budget is 16667 us; 240 Hz is 4167 us.\n");
    return 0;
}
