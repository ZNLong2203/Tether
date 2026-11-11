/*
 * tether-ai — the untrusted AI controller.
 *
 * It reads the observation the arbiter returns, runs the MLP, and proposes a
 * pressure. It is a client of the arbiter: it can be slow, hung, crashed or
 * malicious and the arbiter's control loop does not wait for it.
 *
 * Start-up does what needs privilege, then gives it up for good:
 *   1. as root: create threads at their priorities, attach the control name,
 *      load the model;
 *   2. lock abilities (no spawn, fork, setuid, physical memory, ...), then
 *      setgid/setuid to the unprivileged AI user.
 * After that the kernel refuses this process /dev/tether/actuator.
 *
 * Fault injection (tether-ctl fault NAME) makes it misbehave on purpose:
 *   crash hang hog slow  nan spike drift stuck  bypass
 * SPDX-License-Identifier: Apache-2.0
 */
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/dispatch.h>
#include <sys/neutrino.h>
#include <sys/procmgr.h>
#include <time.h>
#include <unistd.h>

#include "qcommon.h"
#include "tether/common.h"
#include "tether/policy.h"
#include "tether/proto.h"
#include "tether/rt.h"

#define TAG "ai"
#define MAX_HOGS 8
#define HOG_MAX_S 20 /* safety net: a hog never outlives this, so the VM stays reachable */

static struct {
    tether_config cfg;
    tether_mlp model[2];
    _Atomic int active;      /* slot the inference loop reads */
    _Atomic int in_use;      /* slot it is reading right now, -1 if none */
    _Atomic int fault;       /* requested tether_fault */
    _Atomic uint64_t proposals, bypass_attempts, bypass_denied;
    _Atomic int64_t infer_p99_ns, infer_max_ns;
    pthread_mutex_t model_mu;
    char model_path[160];
    int unprivileged;
    int n_hogs;
    pthread_t hog[MAX_HOGS];
    _Atomic int hog_run;
    _Atomic int ctl_ready; /* set once the control name is attached: privileges may be dropped */
    pthread_mutex_t hog_mu;
    pthread_cond_t hog_cv;
} g;

/* ---- model hot swap ------------------------------------------------------ */

static int load_model(const char *path, char *err, size_t errlen)
{
    pthread_mutex_lock(&g.model_mu);
    int target = 1 - atomic_load(&g.active);
    for (int i = 0; i < 50 && atomic_load(&g.in_use) == target; i++) usleep(1000);
    int rc = tether_mlp_load_file(&g.model[target], path, err, errlen);
    if (rc == 0) {
        atomic_store(&g.active, target);
        snprintf(g.model_path, sizeof(g.model_path), "%s", path);
        qlog(TAG, "model %u active (%s)", g.model[target].model_id, path);
    }
    pthread_mutex_unlock(&g.model_mu);
    return rc;
}

/* ---- hog threads: parked until a HOG fault, created before privileges drop --- */

static void *hog_thread(void *arg)
{
    (void)arg;
    for (;;) {
        pthread_mutex_lock(&g.hog_mu);
        while (!atomic_load(&g.hog_run)) pthread_cond_wait(&g.hog_cv, &g.hog_mu);
        pthread_mutex_unlock(&g.hog_mu);
        volatile uint64_t x = 0;
        int64_t end = tether_now_ns() + (int64_t)HOG_MAX_S * TETHER_NS_PER_S;
        while (atomic_load(&g.hog_run) && tether_now_ns() < end) {
            for (int i = 0; i < 20000; i++) x += (uint64_t)i;
        }
        atomic_store(&g.hog_run, 0);
    }
    return NULL;
}

static void set_hog(int on)
{
    pthread_mutex_lock(&g.hog_mu);
    atomic_store(&g.hog_run, on);
    pthread_cond_broadcast(&g.hog_cv);
    pthread_mutex_unlock(&g.hog_mu);
}

/* ---- control channel: faults, model swaps, info -------------------------- */

typedef union {
    uint16_t type;
    tether_msg_hdr hdr;
    tether_msg_fault fault;
    tether_msg_model model;
    struct _pulse pulse;
} ctl_msg;

static void *control_thread(void *arg)
{
    (void)arg;
    name_attach_t *att = name_attach(NULL, TETHER_AI_NAME, 0);
    if (att == NULL) {
        qlog(TAG, "name_attach %s failed: errno %d", TETHER_AI_NAME, errno);
        atomic_store(&g.ctl_ready, -1);
        return NULL;
    }
    atomic_store(&g.ctl_ready, 1);
    static ctl_msg msg;
    for (;;) {
        struct _msg_info info;
        int rcvid = MsgReceive(att->chid, &msg, sizeof(msg), &info);
        if (rcvid < 0) continue;
        if (rcvid == 0) {
            if (msg.pulse.code == _PULSE_CODE_DISCONNECT) ConnectDetach(msg.pulse.scoid);
            continue;
        }
        if (msg.type == _IO_CONNECT) {
            MsgReply(rcvid, EOK, NULL, 0);
            continue;
        }
        if (msg.hdr.type != TETHER_MSG_AI_INFO) {
            /* Only operators may make the AI misbehave or change its model. */
            struct _client_info ci;
            if (ConnectClientInfo(info.scoid, &ci, 0) == -1 ||
                (ci.cred.euid != 0 && ci.cred.euid != TETHER_UID_BRIDGE)) {
                MsgError(rcvid, EPERM);
                continue;
            }
        }
        switch (msg.hdr.type) {
        case TETHER_MSG_FAULT: {
            tether_fault_reply r;
            memset(&r, 0, sizeof(r));
            if (info.msglen < (int)sizeof(tether_msg_fault) || msg.fault.fault < 0 ||
                msg.fault.fault >= TETHER_FAULT_COUNT) {
                r.status = EINVAL;
            } else {
                r.fault = msg.fault.fault;
                r.t_applied_ns = tether_now_ns();
                atomic_store(&g.fault, msg.fault.fault);
                set_hog(msg.fault.fault == TETHER_FAULT_HOG);
                qlog(TAG, "fault requested: %s", tether_fault_name((tether_fault)msg.fault.fault));
            }
            MsgReply(rcvid, (int)sizeof(r), &r, sizeof(r));
            break;
        }
        case TETHER_MSG_MODEL: {
            tether_model_reply r;
            memset(&r, 0, sizeof(r));
            char path[sizeof(msg.model.path)];
            if (info.msglen < (int)sizeof(tether_msg_model)) {
                r.status = EINVAL;
            } else {
                memcpy(path, msg.model.path, sizeof(path));
                path[sizeof(path) - 1] = '\0';
                r.status = load_model(path, r.err, sizeof(r.err)) == 0 ? 0 : EINVAL;
                r.model_id = g.model[atomic_load(&g.active)].model_id;
            }
            MsgReply(rcvid, (int)sizeof(r), &r, sizeof(r));
            break;
        }
        case TETHER_MSG_AI_INFO: {
            tether_ai_info i;
            memset(&i, 0, sizeof(i));
            i.model_id = g.model[atomic_load(&g.active)].model_id;
            i.fault = atomic_load(&g.fault);
            i.proposals = atomic_load(&g.proposals);
            i.bypass_attempts = atomic_load(&g.bypass_attempts);
            i.bypass_denied = atomic_load(&g.bypass_denied);
            i.infer_p99_ns = atomic_load(&g.infer_p99_ns);
            i.infer_max_ns = atomic_load(&g.infer_max_ns);
            i.pid = (int32_t)getpid();
            i.uid = (int32_t)geteuid();
            MsgReply(rcvid, (int)sizeof(i), &i, sizeof(i));
            break;
        }
        default:
            MsgError(rcvid, ENOSYS);
        }
    }
    return NULL;
}

/* ---- giving up privilege ---------------------------------------------------- */

static void drop_privileges(uid_t uid, gid_t gid)
{
    /* Refuse, for good, everything an AI has no business doing. Done as root,
     * before the uid changes, as the procmgr_ability documentation describes. */
    int e = procmgr_ability(0,
                            PROCMGR_ADN_NONROOT | PROCMGR_AOP_DENY | PROCMGR_AOP_LOCK | PROCMGR_AID_SPAWN,
                            PROCMGR_ADN_NONROOT | PROCMGR_AOP_DENY | PROCMGR_AOP_LOCK | PROCMGR_AID_FORK,
                            PROCMGR_ADN_NONROOT | PROCMGR_AOP_DENY | PROCMGR_AOP_LOCK | PROCMGR_AID_SETUID,
                            PROCMGR_ADN_NONROOT | PROCMGR_AOP_DENY | PROCMGR_AOP_LOCK | PROCMGR_AID_SETGID,
                            PROCMGR_ADN_NONROOT | PROCMGR_AOP_DENY | PROCMGR_AOP_LOCK | PROCMGR_AID_SPAWN_SETUID,
                            PROCMGR_ADN_NONROOT | PROCMGR_AOP_DENY | PROCMGR_AOP_LOCK | PROCMGR_AID_SPAWN_SETGID,
                            PROCMGR_ADN_NONROOT | PROCMGR_AOP_DENY | PROCMGR_AOP_LOCK | PROCMGR_AID_MEM_PHYS,
                            PROCMGR_ADN_NONROOT | PROCMGR_AOP_DENY | PROCMGR_AOP_LOCK | PROCMGR_AID_INTERRUPT,
                            PROCMGR_ADN_NONROOT | PROCMGR_AOP_DENY | PROCMGR_AOP_LOCK | PROCMGR_AID_IO,
                            PROCMGR_ADN_NONROOT | PROCMGR_AOP_DENY | PROCMGR_AOP_LOCK | PROCMGR_AID_CLOCKSET,
                            PROCMGR_ADN_NONROOT | PROCMGR_AOP_DENY | PROCMGR_AOP_LOCK | PROCMGR_AID_REBOOT,
                            PROCMGR_ADN_NONROOT | PROCMGR_AOP_DENY | PROCMGR_AOP_LOCK | PROCMGR_AID_RSRCDBMGR,
                            PROCMGR_ADN_NONROOT | PROCMGR_AOP_DENY | PROCMGR_AOP_LOCK | PROCMGR_AID_PATHSPACE,
                            PROCMGR_ADN_NONROOT | PROCMGR_AOP_DENY | PROCMGR_AOP_LOCK | PROCMGR_AID_XPROCESS_DEBUG,
                            PROCMGR_ADN_NONROOT | PROCMGR_AOP_DENY | PROCMGR_AOP_LOCK | PROCMGR_AID_XPROCESS_MEM_READ,
                            PROCMGR_AID_EOL);
    if (e != EOK) qlog(TAG, "procmgr_ability failed: %s", strerror(e));
    if (setgid(gid) != 0 || setuid(uid) != 0) {
        qlog(TAG, "cannot drop to uid %d: %s; refusing to run as root", (int)uid, strerror(errno));
        exit(1);
    }
    g.unprivileged = 1;
    qlog(TAG, "now uid %d gid %d, abilities locked", (int)geteuid(), (int)getegid());
}

/* ---- bypass attempt ----------------------------------------------------------- */

static void try_bypass(void)
{
    atomic_fetch_add(&g.bypass_attempts, 1);
    int fd = open(TETHER_DEV_ACTUATOR, O_WRONLY);
    if (fd < 0) {
        if (atomic_fetch_add(&g.bypass_denied, 1) == 0)
            qlog(TAG, "bypass: open(%s) -> %s", TETHER_DEV_ACTUATOR, strerror(errno));
        return;
    }
    /* Only reachable if the OS let us in (running as root, no isolation). */
    tether_hw_command c = { 70.0, tether_now_ns() };
    devctl(fd, DCMD_TETHER_SET_COMMAND, &c, sizeof(c), NULL);
    close(fd);
}

/* ---- the inference loop --------------------------------------------------------- */

static int connect_arbiter(void)
{
    for (;;) {
        int coid = name_open(TETHER_ARBITER_NAME, 0);
        if (coid >= 0) return coid;
        usleep(200 * 1000);
    }
}

static void add_period(struct timespec *t, int64_t ns)
{
    t->tv_nsec += ns;
    while (t->tv_nsec >= 1000000000L) {
        t->tv_nsec -= 1000000000L;
        t->tv_sec++;
    }
}

static void run(void)
{
    static tether_hist infer;
    tether_hist_reset(&infer);
    tether_fault_state fs;
    tether_fault_arm(&fs, TETHER_FAULT_NONE, 0);
    int armed = TETHER_FAULT_NONE;

    tether_observation obs;
    memset(&obs, 0, sizeof(obs));
    int have_obs = 0;
    uint32_t seq = 0;
    int coid = connect_arbiter();
    qlog(TAG, "connected to %s", TETHER_ARBITER_NAME);

    const int64_t period_ns = (int64_t)llround(1e9 / g.cfg.timing.ai_hz);
    struct timespec next;
    clock_gettime(CLOCK_MONOTONIC, &next);

    for (;;) {
        add_period(&next, period_ns);
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL);

        int want = atomic_load(&g.fault);
        if (want != armed) {
            tether_fault_arm(&fs, (tether_fault)want, obs.breath);
            armed = want;
        }
        if (want == TETHER_FAULT_CRASH) {
            qlog(TAG, "injecting fault: crash (write through NULL)");
            *(volatile int *)0 = 1;
        }
        if (want == TETHER_FAULT_HANG) {
            qlog(TAG, "injecting fault: hang (spinning, no more proposals)");
            for (volatile uint64_t x = 0;; x++) { }
        }
        if (want == TETHER_FAULT_BYPASS) try_bypass();

        int64_t t0 = tether_now_ns();
        double u = g.cfg.vent.peep;
        if (have_obs) {
            int slot = atomic_load(&g.active);
            atomic_store(&g.in_use, slot);
            u = tether_policy_command(&g.model[slot], &obs);
            atomic_store(&g.in_use, -1);
            if (want != TETHER_FAULT_NONE) u = tether_fault_apply(&fs, &obs, u);
        }
        if (want == TETHER_FAULT_SLOW) usleep(40 * 1000); /* inference takes 40 ms */
        int64_t dt = tether_now_ns() - t0;
        tether_hist_add(&infer, dt);
        atomic_store(&g.infer_p99_ns, tether_hist_percentile(&infer, 99.0));
        atomic_store(&g.infer_max_ns, infer.max_ns);

        tether_msg_propose m;
        memset(&m, 0, sizeof(m));
        m.hdr.type = TETHER_MSG_PROPOSE;
        m.seq = ++seq;
        m.model_id = g.model[atomic_load(&g.active)].model_id;
        m.u = u;
        m.t_sent_ns = tether_now_ns();

        uint64_t to = 200 * TETHER_NS_PER_MS; /* never block forever on the arbiter */
        TimerTimeout(CLOCK_MONOTONIC, _NTO_TIMEOUT_SEND | _NTO_TIMEOUT_REPLY, NULL, &to, NULL);
        long r = MsgSend(coid, &m, sizeof(m), &obs, sizeof(obs));
        if (r < 0) {
            int e = errno;
            if (e == EBADF || e == ESRCH || e == ENOENT || e == ESTALE) {
                qlog(TAG, "lost the arbiter (%s); reconnecting", strerror(e));
                name_close(coid);
                coid = connect_arbiter();
            }
            continue;
        }
        have_obs = (r == (long)sizeof(obs));
        atomic_fetch_add(&g.proposals, 1);
    }
}

int main(int argc, char **argv)
{
    memset(&g, 0, sizeof(g));
    if (q_config_load(argc, argv, &g.cfg, TAG) != 0) return 2;
    const char *model = "/data/tether/models/vent-v1.tmlp";
    uid_t uid = TETHER_UID_AI;
    gid_t gid = TETHER_GID_AI;
    int drop = 1;
    for (int i = 1; i < argc; i++) {
        if (strncmp(argv[i], "--model=", 8) == 0) model = argv[i] + 8;
        else if (strcmp(argv[i], "--keep-root") == 0) drop = 0; /* demo: shows the bypass succeeding */
        else if (strncmp(argv[i], "--uid=", 6) == 0) uid = (uid_t)atoi(argv[i] + 6);
    }
    pthread_mutex_init(&g.model_mu, NULL);
    pthread_mutex_init(&g.hog_mu, NULL);
    pthread_cond_init(&g.hog_cv, NULL);
    atomic_store(&g.in_use, -1);

    char err[128];
    if (load_model(model, err, sizeof(err)) != 0) {
        qlog(TAG, "cannot load %s: %s", model, err);
        return 1;
    }

    /* Threads, at their priorities, while we still may. */
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setinheritsched(&at, PTHREAD_EXPLICIT_SCHED);
    pthread_attr_setschedpolicy(&at, SCHED_FIFO);
    struct sched_param sp;
    memset(&sp, 0, sizeof(sp));
    sp.sched_priority = TETHER_PRIO_AI_CTRL;
    pthread_attr_setschedparam(&at, &sp);
    pthread_t th;
    pthread_create(&th, NULL, control_thread, NULL);
    pthread_setschedparam(th, SCHED_FIFO, &sp);
    sp.sched_priority = TETHER_PRIO_AI;
    pthread_attr_setschedparam(&at, &sp);
    long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
    g.n_hogs = ncpu < 1 ? 1 : (ncpu > MAX_HOGS ? MAX_HOGS : (int)ncpu);
    for (int i = 0; i < g.n_hogs; i++) pthread_create(&g.hog[i], &at, hog_thread, NULL);
    q_set_fifo(TETHER_PRIO_AI);
    /* The control thread must have attached its name before we give up the
     * right to create one. Wait for its signal, not for a guessed delay. */
    for (int i = 0; i < 500 && atomic_load(&g.ctl_ready) == 0; i++) usleep(10 * 1000);
    if (atomic_load(&g.ctl_ready) != 1) {
        qlog(TAG, "control channel is not up; refusing to continue");
        return 1;
    }

    if (drop) drop_privileges(uid, gid);
    else qlog(TAG, "running as uid %d: NO privilege drop (demo)", (int)geteuid());

    run();
    return 0;
}
