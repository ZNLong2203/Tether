/*
 * tether-hw — the virtual ventilator hardware, as a QNX resource manager.
 *
 * This is the only simulated part of Tether. It offers what a driver for a
 * real blower and flow sensor would offer, as device nodes:
 *
 *   /dev/tether/sensor    0444  read paw/flow (devctl READ_SENSOR), status
 *   /dev/tether/actuator  0600  root only: set the blower pressure command
 *   /dev/tether/plant     0660  root and group 2000 (operators): change the simulated lung
 *
 * Because the actuator node is root-only, a process running as the AI's user
 * cannot open it: the kernel refuses, not our code. Refusals are counted.
 *
 * A physics thread integrates the lung in real time. A hardware watchdog
 * drops the blower to PEEP if no command arrives for hw_watchdog_s, so a dead
 * arbiter cannot leave the patient pressurised.
 * SPDX-License-Identifier: Apache-2.0
 */
/* Typed handles for the resource manager callbacks (see <sys/resmgr.h>). */
struct _iofunc_attr;
#define RESMGR_HANDLE_T struct _iofunc_attr
struct _iofunc_ocb;
#define RESMGR_OCB_T struct _iofunc_ocb

#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/dispatch.h>
#include <sys/iofunc.h>
#include <sys/neutrino.h>
#include <sys/stat.h>
#include <time.h>

#include "qcommon.h"
#include "tether/common.h"
#include "tether/config.h"
#include "tether/plant.h"
#include "tether/proto.h"

#define TAG "hw"
#define HW_BLOWER_MAX 80.0 /* what the blower can physically deliver, cmH2O */

enum { NODE_SENSOR, NODE_ACTUATOR, NODE_PLANT, NODE_COUNT };

static struct {
    tether_config cfg;
    pthread_mutex_t mu;
    tether_lung lung;
    tether_plant plant;
    double u_cmd;           /* last command from the arbiter */
    double u_applied;       /* what the blower is really given (PEEP if the watchdog tripped) */
    int64_t last_cmd_ns;
    uint64_t commands, sensor_seq;
    uint32_t wd_trips;
    int wd_active;
    int have_cmd;
    _Atomic uint32_t denied_opens;
    iofunc_attr_t attr[NODE_COUNT];
} hw;

/* ---- physics thread --------------------------------------------------- */

static void *physics_thread(void *arg)
{
    (void)arg;
    q_set_fifo(TETHER_PRIO_HW_PHYSICS);
    const int64_t period_ns = (int64_t)(1e9 / hw.cfg.timing.plant_hz);
    struct timespec next;
    clock_gettime(CLOCK_MONOTONIC, &next);
    int64_t last = tether_now_ns();

    for (;;) {
        next.tv_nsec += period_ns;
        while (next.tv_nsec >= 1000000000L) {
            next.tv_nsec -= 1000000000L;
            next.tv_sec++;
        }
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL);

        int64_t now = tether_now_ns();
        double dt = (double)(now - last) * 1e-9;
        last = now;
        if (dt > 0.05) dt = 0.05; /* never integrate across a long stall in one go */

        pthread_mutex_lock(&hw.mu);
        if (hw.have_cmd && !hw.wd_active &&
            (double)(now - hw.last_cmd_ns) * 1e-9 > hw.cfg.timing.hw_watchdog_s) {
            hw.wd_active = 1;
            hw.wd_trips++;
            qlog_rt(TAG, "watchdog: no command for %.0f ms, blower falls back to PEEP",
                 (double)(now - hw.last_cmd_ns) * 1e-6);
        }
        hw.u_applied = hw.wd_active ? hw.cfg.vent.peep : hw.u_cmd;
        tether_plant_step(&hw.plant, &hw.lung, hw.u_applied, dt);
        pthread_mutex_unlock(&hw.mu);
    }
    return NULL;
}

/* ---- resource manager ------------------------------------------------- */

static int reply_data(resmgr_context_t *ctp, io_devctl_t *msg, const void *data, size_t n)
{
    memset(&msg->o, 0, sizeof(msg->o));
    msg->o.ret_val = EOK;
    msg->o.nbytes = (int32_t)n;
    if (n > 0) memcpy(_DEVCTL_DATA(msg->o), data, n);
    return _RESMGR_PTR(ctp, &msg->o, sizeof(msg->o) + n);
}

static int hw_devctl(resmgr_context_t *ctp, io_devctl_t *msg, iofunc_ocb_t *ocb)
{
    int s = iofunc_devctl_default(ctp, msg, ocb);
    if (s != _RESMGR_DEFAULT) return s;

    const iofunc_attr_t *a = ocb->attr;
    const int node = (int)(a - hw.attr);
    const int dcmd = msg->i.dcmd;

    if (dcmd == DCMD_TETHER_READ_SENSOR && node == NODE_SENSOR) {
        tether_hw_sensor out;
        pthread_mutex_lock(&hw.mu);
        tether_sensor_reading r = tether_plant_read(&hw.plant, &hw.lung);
        out.seq = ++hw.sensor_seq;
        pthread_mutex_unlock(&hw.mu);
        out.paw = r.paw;
        out.flow = r.flow;
        out.t_ns = tether_now_ns();
        return reply_data(ctp, msg, &out, sizeof(out));
    }

    if (dcmd == DCMD_TETHER_SET_COMMAND && node == NODE_ACTUATOR) {
        const tether_hw_command *in = _DEVCTL_DATA(msg->i);
        if (!isfinite(in->u)) return EINVAL;
        double u = in->u < 0.0 ? 0.0 : (in->u > HW_BLOWER_MAX ? HW_BLOWER_MAX : in->u);
        pthread_mutex_lock(&hw.mu);
        hw.u_cmd = u;
        hw.last_cmd_ns = tether_now_ns();
        hw.have_cmd = 1;
        if (hw.wd_active) {
            hw.wd_active = 0;
            qlog_rt(TAG, "watchdog cleared: commands are arriving again");
        }
        hw.commands++;
        pthread_mutex_unlock(&hw.mu);
        return reply_data(ctp, msg, NULL, 0);
    }

    if (dcmd == DCMD_TETHER_HW_STATUS && (node == NODE_SENSOR || node == NODE_PLANT)) {
        tether_hw_status st;
        memset(&st, 0, sizeof(st));
        pthread_mutex_lock(&hw.mu);
        st.lung = hw.lung;
        st.u_applied = hw.u_applied;
        st.commands = hw.commands;
        st.watchdog_trips = hw.wd_trips;
        st.watchdog_active = hw.wd_active;
        pthread_mutex_unlock(&hw.mu);
        st.denied_opens = atomic_load(&hw.denied_opens);
        return reply_data(ctp, msg, &st, sizeof(st));
    }

    if (dcmd == DCMD_TETHER_SET_LUNG && node == NODE_PLANT) {
        const tether_lung *in = _DEVCTL_DATA(msg->i);
        if (!(in->resistance > 0 && in->compliance > 0 && in->blower_tau > 0)) return EINVAL;
        pthread_mutex_lock(&hw.mu);
        hw.lung = *in;
        pthread_mutex_unlock(&hw.mu);
        qlog(TAG, "lung changed: R=%.1f C=%.3f", in->resistance, in->compliance);
        return reply_data(ctp, msg, NULL, 0);
    }

    return ENOSYS;
}

/* Count opens the kernel's permission check refuses (EACCES), then fail them. */
static int hw_open(resmgr_context_t *ctp, io_open_t *msg, RESMGR_HANDLE_T *handle, void *extra)
{
    int r = iofunc_open_default(ctp, msg, handle, extra);
    if (r == EACCES) {
        /* Counted always, logged rarely and only to memory: an AI that spams
         * refused opens must not be able to slow this high-priority thread
         * down with disk writes. */
        uint32_t n = atomic_fetch_add(&hw.denied_opens, 1) + 1;
        if (n == 1 || n % 64 == 0) {
            struct _client_info ci;
            int uid = -1;
            if (ConnectClientInfo(ctp->info.scoid, &ci, 0) != -1) uid = (int)ci.cred.euid;
            qlog_rt(TAG, "open refused by the OS (uid %d, pid %d): %u so far", uid, (int)ctp->info.pid, n);
        }
    }
    return r;
}

int main(int argc, char **argv)
{
    memset(&hw, 0, sizeof(hw));
    if (q_config_load(argc, argv, &hw.cfg, TAG) != 0) return 2;

    for (int i = 1; i < argc; i++) {
        if (strncmp(argv[i], "--tick-us=", 10) == 0) {
            int e = q_clock_period_us((unsigned)atoi(argv[i] + 10));
            if (e) qlog(TAG, "could not set the clock period: errno %d", e);
        }
    }

    pthread_mutexattr_t ma;
    pthread_mutexattr_init(&ma);
    pthread_mutexattr_setprotocol(&ma, PTHREAD_PRIO_INHERIT); /* no priority inversion */
    pthread_mutex_init(&hw.mu, &ma);

    tether_lung_defaults(&hw.lung);
    tether_plant_init(&hw.plant, &hw.lung, hw.cfg.vent.peep, 1);
    hw.u_cmd = hw.u_applied = hw.cfg.vent.peep;

    dispatch_t *dpp = dispatch_create();
    if (dpp == NULL) {
        qlog(TAG, "dispatch_create failed: errno %d", errno);
        return 1;
    }
    resmgr_attr_t rattr;
    memset(&rattr, 0, sizeof(rattr));
    rattr.nparts_max = 1;
    rattr.msg_max_size = 1024;

    resmgr_connect_funcs_t cfuncs;
    resmgr_io_funcs_t ifuncs;
    iofunc_func_init(_RESMGR_CONNECT_NFUNCS, &cfuncs, _RESMGR_IO_NFUNCS, &ifuncs);
    cfuncs.open = hw_open;
    ifuncs.devctl = hw_devctl;

    static const struct { const char *path; mode_t mode; } nodes[NODE_COUNT] = {
        [NODE_SENSOR] = { TETHER_DEV_SENSOR, S_IFCHR | 0444 },
        [NODE_ACTUATOR] = { TETHER_DEV_ACTUATOR, S_IFCHR | 0600 },
        [NODE_PLANT] = { TETHER_DEV_PLANT, S_IFCHR | 0660 }, /* group: operators (the dashboard) */
    };
    for (int i = 0; i < NODE_COUNT; i++) {
        iofunc_attr_init(&hw.attr[i], nodes[i].mode, NULL, NULL);
        hw.attr[i].uid = 0;
        hw.attr[i].gid = (i == NODE_PLANT) ? TETHER_GID_OPS : 0;
        if (resmgr_attach(dpp, &rattr, nodes[i].path, _FTYPE_ANY, 0, &cfuncs, &ifuncs, &hw.attr[i]) == -1) {
            qlog(TAG, "resmgr_attach %s failed: errno %d", nodes[i].path, errno);
            return 1;
        }
    }

    q_lock_memory();
    pthread_t th;
    pthread_create(&th, NULL, physics_thread, NULL);
    q_set_fifo(TETHER_PRIO_HW_RESMGR);

    qlog(TAG, "ready: %s %s %s; plant %.0f Hz, watchdog %.0f ms", TETHER_DEV_SENSOR, TETHER_DEV_ACTUATOR,
         TETHER_DEV_PLANT, hw.cfg.timing.plant_hz, hw.cfg.timing.hw_watchdog_s * 1e3);

    dispatch_context_t *ctp = dispatch_context_alloc(dpp);
    for (;;) {
        if ((ctp = dispatch_block(ctp)) == NULL) {
            qlog(TAG, "dispatch_block failed: errno %d", errno);
            return 1;
        }
        dispatch_handler(ctp);
    }
}
