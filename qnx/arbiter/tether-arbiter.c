/*
 * tether-arbiter — the runtime-assurance process.
 *
 * Three kinds of threads, by priority:
 *   control  (60)  woken by a timer pulse every 1/control_hz s: reads the
 *                  sensor, runs the safety monitor and switch, writes the
 *                  actuator. It never waits for the AI and never allocates.
 *   ipc      (50)  server for the AI and the dashboard: MsgReceive on a named
 *                  channel. Hands proposals to the control thread through a
 *                  seqlock mailbox; answers questions from snapshots.
 *   logger   (35)  polls hardware status, logs switch events and a summary.
 *
 * The AI is always a client: it MsgSend()s a proposal and gets the current
 * observation back in the reply. If it hangs, only its own send blocks. If it
 * dies, the kernel sends a disconnect pulse and the next tick falls back to
 * the verified recovery controller.
 * SPDX-License-Identifier: Apache-2.0
 */
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/dispatch.h>
#include <sys/neutrino.h>
#include <sys/siginfo.h>
#include <time.h>
#include <unistd.h>

#include "qcommon.h"
#include "tether/arbiter.h"
#include "tether/common.h"
#include "tether/proto.h"
#include "tether/rt.h"

#define TAG "arbiter"
enum { PULSE_TICK = _PULSE_CODE_MINAVAIL + 1 };

/* Everything the control thread publishes for the other threads, copied in one
 * go every 50 ms. The other threads turn it into a tether_status on demand,
 * so the percentile arithmetic never runs at control priority. */
typedef struct {
    tether_arbiter arb;
    tether_hist wake, exec;
    tether_eventlog elog;
    tether_proposal last;
    int64_t now_ns;
    int32_t connected;
    int32_t pad;
} big_snap;

Q_SNAPSHOT(obs_snap_t, tether_observation);
Q_SNAPSHOT(big_snap_t, big_snap);

static struct {
    tether_config cfg;
    int sensor_fd, act_fd;
    tether_mailbox mbox;
    tether_ring ring;
    obs_snap_t obs;
    big_snap_t big;
    _Atomic int ai_connected, ai_scoid, ai_pid, ai_ever;
    _Atomic uint32_t ai_restarts, hw_trips, hw_denied;
    _Atomic int guard_off;
    _Atomic int reset_req;
    _Atomic uint64_t sensor_errors, actuator_errors, overruns;
    pthread_mutex_t status_mu;
} g;

/* ---- control thread ----------------------------------------------------- */

static void arm_timer(timer_t tid, int64_t abs_ns)
{
    struct itimerspec it;
    memset(&it, 0, sizeof(it));
    it.it_value.tv_sec = abs_ns / TETHER_NS_PER_S;
    it.it_value.tv_nsec = abs_ns % TETHER_NS_PER_S;
    timer_settime(tid, TIMER_ABSTIME, &it, NULL);
}

static big_snap cs; /* scratch for the control thread's copy (static: keeps the stack small) */

static void *control_thread(void *arg)
{
    (void)arg;
    if (q_set_fifo(TETHER_PRIO_CONTROL) != 0) qlog(TAG, "control thread is not real-time (no SCHED_FIFO)");

    const int64_t period_ns = (int64_t)llround(1e9 / g.cfg.timing.control_hz);
    const uint64_t publish_every = (uint64_t)(g.cfg.timing.control_hz / 20.0) + 1;

    int chid = ChannelCreate(0);
    int coid = ConnectAttach(0, 0, chid, _NTO_SIDE_CHANNEL, 0);
    struct sigevent ev;
    SIGEV_PULSE_INIT(&ev, coid, TETHER_PRIO_CONTROL, PULSE_TICK, 0);
    timer_t tid;
    if (chid < 0 || coid < 0 || timer_create(CLOCK_MONOTONIC, &ev, &tid) != 0) {
        qlog(TAG, "cannot create the control timer: errno %d", errno);
        exit(1);
    }

    static tether_arbiter arb;
    static tether_hist wake_h, exec_h;
    static tether_eventlog elog;
    tether_arbiter_init(&arb, &g.cfg);
    tether_hist_reset(&wake_h);
    tether_hist_reset(&exec_h);
    memset(&elog, 0, sizeof(elog));
    tether_proposal last_p;
    memset(&last_p, 0, sizeof(last_p));
    double raw_u = g.cfg.vent.peep;

    /* One-shot timers at absolute times on a fixed grid: lateness is then true
     * scheduling jitter, and it does not accumulate the way a periodic timer
     * does when the kernel tick is coarser than the period. */
    int64_t ideal = tether_now_ns() + period_ns;
    arm_timer(tid, ideal);
    qlog(TAG, "control loop running at %.0f Hz", g.cfg.timing.control_hz);

    uint64_t k = 0;
    for (;;) {
        struct _pulse pulse;
        int rcvid = MsgReceive(chid, &pulse, sizeof(pulse), NULL);
        if (rcvid != 0 || pulse.code != PULSE_TICK) continue;

        if (atomic_exchange_explicit(&g.reset_req, 0, memory_order_relaxed)) {
            tether_hist_reset(&wake_h); /* a new measurement window */
            tether_hist_reset(&exec_h);
        }
        const int64_t now = tether_now_ns();
        int64_t wake = now - ideal;
        if (wake < 0) wake = 0;
        tether_hist_add(&wake_h, wake);
        k++;
        ideal += period_ns;
        if (now >= ideal) { /* more than a whole period late */
            int64_t behind = (now - ideal) / period_ns + 1;
            if (behind > 10) { /* hopelessly late: give up catching up, and say so */
                atomic_fetch_add_explicit(&g.overruns, (uint64_t)behind, memory_order_relaxed);
                ideal = now + period_ns;
            }
        }
        arm_timer(tid, ideal);

        double paw = NAN, flow = NAN;
        tether_hw_sensor s;
        if (devctl(g.sensor_fd, DCMD_TETHER_READ_SENSOR, &s, sizeof(s), NULL) == EOK) {
            paw = s.paw;
            flow = s.flow;
        } else {
            atomic_fetch_add_explicit(&g.sensor_errors, 1, memory_order_relaxed);
        }

        tether_proposal p;
        if (tether_mailbox_read(&g.mbox, &p)) last_p = p;
        const int connected = atomic_load_explicit(&g.ai_connected, memory_order_acquire);

        tether_decision d = tether_arbiter_step(&arb, paw, flow, last_p.valid ? &last_p : NULL, now, connected);

        double u = d.u;
        if (atomic_load_explicit(&g.guard_off, memory_order_relaxed)) {
            /* Demo pass-through: whatever the AI said last goes to the blower. */
            if (last_p.valid && isfinite(last_p.u)) raw_u = last_p.u;
            u = raw_u;
        }
        tether_hw_command c = { u, now };
        if (devctl(g.act_fd, DCMD_TETHER_SET_COMMAND, &c, sizeof(c), NULL) != EOK)
            atomic_fetch_add_explicit(&g.actuator_errors, 1, memory_order_relaxed);

        if (d.switched) tether_eventlog_push(&elog, &arb.last_switch);

        tether_sample sm;
        memset(&sm, 0, sizeof(sm));
        sm.tick = arb.tick;
        sm.t_ns = now;
        sm.paw = (float)paw;
        sm.flow = (float)flow;
        sm.vol = (float)arb.vol;
        sm.u = (float)u;
        sm.u_ai = (float)d.u_ai;
        sm.wake_ns = wake > INT32_MAX ? INT32_MAX : (int32_t)wake;
        sm.breath = arb.breath;
        sm.mode = (uint8_t)d.mode;
        sm.verdict = (uint8_t)d.verdict;
        sm.phase = (uint8_t)arb.phase;
        sm.violation = (uint8_t)d.violation;
        tether_ring_push(&g.ring, &sm);

        tether_observation o;
        tether_arbiter_observe(&arb, now, &o);
        q_snap_write(&g.obs, &o);

        const int64_t done = tether_now_ns();
        tether_hist_add(&exec_h, done - now);

        if (k % publish_every == 0) {
            cs.arb = arb;
            cs.wake = wake_h;
            cs.exec = exec_h;
            cs.elog = elog;
            cs.last = last_p;
            cs.now_ns = done;
            cs.connected = connected;
            q_snap_write(&g.big, &cs);
        }
    }
    return NULL;
}

/* ---- status, built outside the control thread --------------------------- */

static int build_status(tether_status *out)
{
    static big_snap bs;
    static tether_status st;
    int ok = 0;
    pthread_mutex_lock(&g.status_mu);
    if (q_snap_read(&g.big, &bs) && bs.arb.tick > 0) {
        tether_status_fill(&st, &bs.arb, &bs.wake, &bs.exec, &bs.elog, &bs.last, bs.now_ns, bs.connected);
        st.ai_restarts = atomic_load(&g.ai_restarts);
        st.hw_watchdog_trips = atomic_load(&g.hw_trips);
        st.hw_denied_opens = atomic_load(&g.hw_denied);
        st.guard_off = (uint32_t)atomic_load(&g.guard_off);
        st.ctl_overruns = atomic_load(&g.overruns);
        *out = st;
        ok = 1;
    }
    pthread_mutex_unlock(&g.status_mu);
    return ok;
}

/* ---- logger thread ------------------------------------------------------ */

static void *logger_thread(void *arg)
{
    (void)arg;
    q_set_fifo(TETHER_PRIO_LOGGER);
    int fd = open(TETHER_DEV_SENSOR, O_RDONLY);
    uint64_t seen = 0;
    int ticks = 0;
    for (;;) {
        usleep(100 * 1000);
        if (fd >= 0) {
            tether_hw_status hs;
            if (devctl(fd, DCMD_TETHER_HW_STATUS, &hs, sizeof(hs), NULL) == EOK) {
                atomic_store(&g.hw_trips, hs.watchdog_trips);
                atomic_store(&g.hw_denied, hs.denied_opens);
            }
        }
        tether_status st;
        if (!build_status(&st)) continue;

        if (st.event_seq > seen) {
            uint64_t fresh = st.event_seq - seen;
            if (fresh > TETHER_EVENT_LOG) fresh = TETHER_EVENT_LOG;
            for (uint64_t i = fresh; i > 0; i--) { /* oldest first */
                const tether_switch_event *e = &st.events[i - 1];
                qlog(TAG, "switch to %s (%s) at breath %u", tether_mode_name((tether_mode)e->to_mode),
                     tether_reason_name((tether_reason)e->reason), e->breath);
            }
            seen = st.event_seq;
        }
        if (++ticks % 50 == 0) {
            qlog(TAG, "mode=%s breath=%u vt=%.3f L dp=%.1f | wake p50=%lld p99=%lld max=%lld us | exec p99=%lld us | "
                      "violations=%llu hw_trips=%u refused=%u overruns=%llu",
                 tether_mode_name((tether_mode)st.mode), st.breath, st.vt_last, st.dp_last,
                 (long long)(st.wake_p50_ns / 1000), (long long)(st.wake_p99_ns / 1000),
                 (long long)(st.wake_max_ns / 1000), (long long)(st.exec_p99_ns / 1000),
                 (unsigned long long)st.violations, st.hw_watchdog_trips, st.hw_denied_opens,
                 (unsigned long long)st.ctl_overruns);
        }
    }
    return NULL;
}

/* ---- IPC server --------------------------------------------------------- */

typedef union {
    uint16_t type;
    tether_msg_hdr hdr;
    tether_msg_propose propose;
    tether_msg_telemetry telemetry;
    tether_msg_guard guard;
    struct _pulse pulse;
} ipc_msg;

static int client_uid(int scoid)
{
    struct _client_info ci;
    if (ConnectClientInfo(scoid, &ci, 0) == -1) return -1;
    return (int)ci.cred.euid;
}

static void handle_propose(int rcvid, const ipc_msg *m, const struct _msg_info *info)
{
    if (info->msglen < (int)sizeof(tether_msg_propose)) {
        MsgError(rcvid, EBADMSG);
        return;
    }
    int uid = client_uid(info->scoid);
    if (uid != TETHER_UID_AI && uid != 0) {
        qlog(TAG, "proposal from uid %d refused", uid);
        MsgError(rcvid, EPERM);
        return;
    }
    if (atomic_load(&g.ai_scoid) != info->scoid || !atomic_load(&g.ai_connected)) {
        if (atomic_exchange(&g.ai_ever, 1)) atomic_fetch_add(&g.ai_restarts, 1);
        atomic_store(&g.ai_scoid, info->scoid);
        atomic_store(&g.ai_pid, info->pid);
        qlog(TAG, "AI connected: pid %d, uid %d", (int)info->pid, uid);
    }
    tether_proposal p;
    p.u = m->propose.u;
    p.seq = m->propose.seq;
    p.model_id = m->propose.model_id;
    p.recv_ns = tether_now_ns();
    p.valid = 1;
    tether_mailbox_write(&g.mbox, &p);
    atomic_store_explicit(&g.ai_connected, 1, memory_order_release);

    tether_observation o;
    if (!q_snap_read(&g.obs, &o)) {
        MsgError(rcvid, EAGAIN);
        return;
    }
    MsgReply(rcvid, (int)sizeof(o), &o, sizeof(o));
}

/* MsgSend() returns the status passed to MsgReply(), so every reply here passes
 * its own length as the status: the client then gets the byte count back. */
static void handle_telemetry(int rcvid, const ipc_msg *m, const struct _msg_info *info)
{
    static struct {
        tether_telemetry_reply h;
        tether_sample s[TETHER_TELEMETRY_MAX];
    } r;
    if (info->msglen < (int)sizeof(tether_msg_telemetry)) {
        MsgError(rcvid, EBADMSG);
        return;
    }
    uint64_t cursor = m->telemetry.cursor;
    uint32_t max = m->telemetry.max;
    if (max == 0 || max > TETHER_TELEMETRY_MAX) max = TETHER_TELEMETRY_MAX;
    r.h.n = (uint32_t)tether_ring_read(&g.ring, &cursor, r.s, max);
    r.h.next_cursor = cursor;
    r.h.pad = 0;
    size_t n = sizeof(r.h) + r.h.n * sizeof(tether_sample);
    MsgReply(rcvid, (int)n, &r, n);
}

static void ipc_loop(void)
{
    name_attach_t *att = name_attach(NULL, TETHER_ARBITER_NAME, 0);
    if (att == NULL) {
        qlog(TAG, "name_attach %s failed: errno %d", TETHER_ARBITER_NAME, errno);
        exit(1);
    }
    qlog(TAG, "serving %s", TETHER_ARBITER_NAME);

    static ipc_msg msg;
    for (;;) {
        struct _msg_info info;
        int rcvid = MsgReceive(att->chid, &msg, sizeof(msg), &info);
        if (rcvid < 0) {
            if (errno == EINTR) continue;
            qlog(TAG, "MsgReceive failed: errno %d", errno);
            continue;
        }
        if (rcvid == 0) { /* pulse */
            if (msg.pulse.code == _PULSE_CODE_DISCONNECT) {
                if (msg.pulse.scoid == atomic_load(&g.ai_scoid) && atomic_load(&g.ai_connected)) {
                    atomic_store_explicit(&g.ai_connected, 0, memory_order_release);
                    qlog(TAG, "AI disconnected (kernel disconnect pulse)");
                }
                ConnectDetach(msg.pulse.scoid);
            }
            continue;
        }
        if (msg.type == _IO_CONNECT) { /* name_open() */
            MsgReply(rcvid, EOK, NULL, 0);
            continue;
        }
        if (msg.type > _IO_BASE && msg.type <= _IO_MAX) {
            MsgError(rcvid, ENOSYS);
            continue;
        }
        switch (msg.hdr.type) {
        case TETHER_MSG_PROPOSE:
            handle_propose(rcvid, &msg, &info);
            break;
        case TETHER_MSG_STATUS: {
            static tether_status st;
            if (build_status(&st)) MsgReply(rcvid, (int)sizeof(st), &st, sizeof(st));
            else MsgError(rcvid, EAGAIN);
            break;
        }
        case TETHER_MSG_TELEMETRY:
            handle_telemetry(rcvid, &msg, &info);
            break;
        case TETHER_MSG_RESET_STATS: {
            if (client_uid(info.scoid) != 0) {
                MsgError(rcvid, EPERM);
                break;
            }
            atomic_store(&g.reset_req, 1);
            int32_t reply = 1;
            MsgReply(rcvid, (int)sizeof(reply), &reply, sizeof(reply));
            break;
        }
        case TETHER_MSG_GUARD: {
            if (info.msglen < (int)sizeof(tether_msg_guard) || client_uid(info.scoid) != 0) {
                MsgError(rcvid, EPERM);
                break;
            }
            int off = msg.guard.enable ? 0 : 1;
            atomic_store(&g.guard_off, off);
            qlog(TAG, "guard %s", off ? "OFF: the AI now drives the blower directly (demo)" : "ON");
            int32_t reply = !off;
            MsgReply(rcvid, (int)sizeof(reply), &reply, sizeof(reply));
            break;
        }
        default:
            MsgError(rcvid, ENOSYS);
        }
    }
}

/* ---- start-up ----------------------------------------------------------- */

static int open_retry(const char *path, int flags)
{
    for (int i = 0; i < 100; i++) {
        int fd = open(path, flags);
        if (fd >= 0) return fd;
        usleep(100 * 1000);
    }
    return -1;
}

int main(int argc, char **argv)
{
    memset(&g, 0, sizeof(g));
    if (q_config_load(argc, argv, &g.cfg, TAG) != 0) return 2;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--passthrough") == 0) atomic_store(&g.guard_off, 1);
        else if (strncmp(argv[i], "--tick-us=", 10) == 0) q_clock_period_us((unsigned)atoi(argv[i] + 10));
    }

    pthread_mutexattr_t ma;
    pthread_mutexattr_init(&ma);
    pthread_mutexattr_setprotocol(&ma, PTHREAD_PRIO_INHERIT);
    pthread_mutex_init(&g.status_mu, &ma);

    tether_mailbox_init(&g.mbox);
    tether_ring_init(&g.ring);
    int e = q_lock_memory();
    if (e) qlog(TAG, "mlockall failed: errno %d", e);

    g.sensor_fd = open_retry(TETHER_DEV_SENSOR, O_RDONLY);
    g.act_fd = open_retry(TETHER_DEV_ACTUATOR, O_WRONLY);
    if (g.sensor_fd < 0 || g.act_fd < 0) {
        qlog(TAG, "virtual hardware not available (is tether-hw running, and are we root?)");
        return 1;
    }

    pthread_t th;
    pthread_create(&th, NULL, logger_thread, NULL);
    pthread_create(&th, NULL, control_thread, NULL);
    q_set_fifo(TETHER_PRIO_ARB_IPC);
    ipc_loop();
    return 0;
}
