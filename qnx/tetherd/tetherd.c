/*
 * tetherd — starts the Tether processes in order and keeps them alive.
 *
 *   tether-hw  ->  tether-arbiter  ->  tether-ai  ->  tether-bridge
 *
 * The AI is the part expected to fail, so it is the part that gets restarted:
 *   - it exits or crashes: restart after a short back-off (the arbiter already
 *     fell back to the recovery controller when the kernel reported the
 *     disconnect);
 *   - it hangs: the arbiter's status shows the last proposal getting older, and
 *     past hang_kill_s we send SIGKILL, which turns a hang into a crash.
 * If hw or the arbiter dies, nothing can be assumed: stop everything and start
 * again from the top.
 *
 * Options for every child come from the "flags" file next to the binaries, so
 * they all see the same configuration.
 * SPDX-License-Identifier: Apache-2.0
 */
#include <errno.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/dispatch.h>
#include <sys/neutrino.h>
#include <sys/wait.h>
#include <unistd.h>

#include "qcommon.h"
#include "tether/common.h"
#include "tether/proto.h"

#define TAG "tetherd"
#define MAX_ARGS 24

typedef struct {
    const char *name;
    pid_t pid;
    int64_t started_ns;
    int recent_exits;
    int64_t window_ns;
} child;

static char g_dir[128] = "/data/tether";
static char *g_flags[MAX_ARGS];
static int g_nflags;
static volatile sig_atomic_t g_stop;

static void on_term(int sig) { (void)sig; g_stop = 1; }

static void load_flags(void)
{
    char path[200], line[512];
    snprintf(path, sizeof(path), "%s/flags", g_dir);
    FILE *f = fopen(path, "r");
    static char store[512];
    if (f != NULL && fgets(line, sizeof(line), f) != NULL) {
        snprintf(store, sizeof(store), "%s", line);
        for (char *tok = strtok(store, " \t\r\n"); tok != NULL && g_nflags < MAX_ARGS - 2; tok = strtok(NULL, " \t\r\n"))
            g_flags[g_nflags++] = tok;
    }
    if (f != NULL) fclose(f);
}

static pid_t start(const char *name)
{
    char path[200];
    snprintf(path, sizeof(path), "%s/bin/%s", g_dir, name);
    char *argv[MAX_ARGS + 2];
    int n = 0;
    argv[n++] = path;
    for (int i = 0; i < g_nflags; i++) argv[n++] = g_flags[i];
    argv[n] = NULL;
    pid_t pid;
    int e = posix_spawn(&pid, path, NULL, NULL, argv, environ);
    if (e != 0) {
        qlog(TAG, "cannot start %s: %s", name, strerror(e));
        return -1;
    }
    qlog(TAG, "started %s (pid %d)", name, (int)pid);
    return pid;
}

static void kill_all(child *c, int n)
{
    for (int i = n - 1; i >= 0; i--) {
        if (c[i].pid > 0) kill(c[i].pid, SIGTERM);
    }
    usleep(300 * 1000);
    for (int i = n - 1; i >= 0; i--) {
        if (c[i].pid > 0) {
            kill(c[i].pid, SIGKILL);
            waitpid(c[i].pid, NULL, 0);
            c[i].pid = 0;
        }
    }
}

/* Asks the arbiter how old the AI's last proposal is. Returns -1 if unknown. */
static int64_t ai_silence_ns(void)
{
    static int coid = -1;
    if (coid < 0) coid = name_open(TETHER_ARBITER_NAME, 0);
    if (coid < 0) return -1;
    tether_msg_hdr h = { TETHER_MSG_STATUS, 0, 0 };
    static tether_status st;
    uint64_t to = 300 * TETHER_NS_PER_MS;
    TimerTimeout(CLOCK_MONOTONIC, _NTO_TIMEOUT_SEND | _NTO_TIMEOUT_REPLY, NULL, &to, NULL);
    long r = MsgSend(coid, &h, sizeof(h), &st, sizeof(st));
    if (r != (long)sizeof(st)) {
        if (r < 0 && (errno == EBADF || errno == ESRCH || errno == ENOENT || errno == ESTALE)) {
            name_close(coid);
            coid = -1;
        }
        return -1;
    }
    return st.ai_connected ? st.ai_age_ns : -1;
}

int main(int argc, char **argv)
{
    double hang_kill_s = 1.0;
    int with_bridge = 1;
    for (int i = 1; i < argc; i++) {
        if (strncmp(argv[i], "--dir=", 6) == 0) snprintf(g_dir, sizeof(g_dir), "%s", argv[i] + 6);
        else if (strncmp(argv[i], "--hang-kill-s=", 14) == 0) hang_kill_s = atof(argv[i] + 14);
        else if (strcmp(argv[i], "--no-bridge") == 0) with_bridge = 0;
    }
    load_flags();
    q_set_fifo(TETHER_PRIO_SUPERVISOR);

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_term;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);

    enum { HW, ARB, AI, BRIDGE, NCHILD };
    child c[NCHILD] = { { .name = "tether-hw" }, { .name = "tether-arbiter" }, { .name = "tether-ai" }, { .name = "tether-bridge" } };
    int ai_restarts = 0;

    for (;;) {
        /* (re)start from the top */
        for (int i = 0; i < NCHILD && !g_stop; i++) {
            if (i == BRIDGE && !with_bridge) continue;
            c[i].pid = start(c[i].name);
            c[i].started_ns = tether_now_ns();
            if (i == HW || i == ARB) sleep(1); /* let the device nodes and the name appear */
        }

        int restart_all = 0;
        while (!g_stop && !restart_all) {
            usleep(100 * 1000);
            int status;
            pid_t dead;
            while ((dead = waitpid(-1, &status, WNOHANG)) > 0) {
                for (int i = 0; i < NCHILD; i++) {
                    if (c[i].pid != dead) continue;
                    c[i].pid = 0;
                    if (WIFSIGNALED(status)) qlog(TAG, "%s died from signal %d", c[i].name, WTERMSIG(status));
                    else qlog(TAG, "%s exited with status %d", c[i].name, WEXITSTATUS(status));
                    if (i == HW || i == ARB) restart_all = 1;
                    else c[i].started_ns = tether_now_ns() + 500 * TETHER_NS_PER_MS; /* back-off */
                }
            }
            /* AI and bridge come back on their own after a back-off. */
            for (int i = AI; i < NCHILD && !restart_all; i++) {
                if (i == BRIDGE && !with_bridge) continue;
                if (c[i].pid <= 0 && tether_now_ns() >= c[i].started_ns) {
                    c[i].pid = start(c[i].name);
                    c[i].started_ns = tether_now_ns();
                    if (i == AI) ai_restarts++;
                }
            }
            /* A hung AI never exits: kill it so it becomes a crash. */
            static int tick;
            /* Test switch: while this file exists a silent AI is left alone, so a
             * measurement can watch the system under a sustained CPU hog. */
            char sw[200];
            snprintf(sw, sizeof(sw), "%s/no-hang-kill", g_dir);
            if (++tick % 3 == 0 && c[AI].pid > 0 && access(sw, F_OK) != 0 && tether_now_ns() - c[AI].started_ns > 3 * TETHER_NS_PER_S) {
                int64_t silent = ai_silence_ns();
                if (silent > (int64_t)(hang_kill_s * 1e9)) {
                    qlog(TAG, "AI silent for %.1f s: killing pid %d", (double)silent * 1e-9, (int)c[AI].pid);
                    kill(c[AI].pid, SIGKILL);
                }
            }
        }
        if (g_stop) break;
        qlog(TAG, "hw or arbiter died: restarting everything");
        kill_all(c, NCHILD);
    }
    qlog(TAG, "stopping (AI restarts so far: %d)", ai_restarts);
    kill_all(c, NCHILD);
    return 0;
}
