/*
 * tether-ctl — talk to the Tether virtual hardware and the arbiter from a shell.
 *
 *   tether-ctl sensor               read airway pressure and flow
 *   tether-ctl hw                   hardware status (watchdog, denied opens, lung)
 *   tether-ctl actuate <cmH2O>      write a blower command (root only)
 *   tether-ctl lung healthy|ards|asthma   change the simulated patient (root only)
 *   tether-ctl status               arbiter status as JSON
 *   tether-ctl fault <name>         tell the AI process to misbehave (see --help)
 *
 * It also serves as the "attacker" in the demo: run as the AI's user it shows
 * that the kernel refuses to open the actuator.
 * SPDX-License-Identifier: Apache-2.0
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/dispatch.h>
#include <sys/neutrino.h>
#include <unistd.h>

#include "qcommon.h"
#include "tether/common.h"
#include "tether/proto.h"

static int open_dev(const char *path, int flags)
{
    int fd = open(path, flags);
    if (fd < 0) fprintf(stderr, "open %s: %s\n", path, strerror(errno));
    return fd;
}

static int call(int fd, int dcmd, void *data, size_t n)
{
    int e = devctl(fd, dcmd, data, n, NULL);
    if (e != EOK) fprintf(stderr, "devctl: %s\n", strerror(e));
    return e;
}

/* One send to a named server with a bounded wait: a stuck server must not
 * hang this tool. */
static int send_named(const char *name, const void *msg, size_t n, void *reply, size_t rn)
{
    int coid = name_open(name, 0);
    if (coid < 0) {
        fprintf(stderr, "name_open %s: %s\n", name, strerror(errno));
        return -1;
    }
    uint64_t timeout = 500 * TETHER_NS_PER_MS;
    TimerTimeout(CLOCK_MONOTONIC, _NTO_TIMEOUT_SEND | _NTO_TIMEOUT_REPLY, NULL, &timeout, NULL);
    long r = MsgSend(coid, msg, n, reply, rn);
    int e = errno;
    name_close(coid);
    if (r < 0) {
        fprintf(stderr, "no answer from %s: %s\n", name, strerror(e));
        return -1;
    }
    return (int)r;
}

static void usage(void)
{
    fputs("usage: tether-ctl sensor | hw | actuate <cmH2O> | lung <healthy|ards|asthma> |\n"
          "                  status | fault <name>\n"
          "faults: ", stderr);
    for (int f = 0; f < TETHER_FAULT_COUNT; f++) fprintf(stderr, "%s ", tether_fault_name((tether_fault)f));
    fputc('\n', stderr);
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        usage();
        return 2;
    }
    const char *cmd = argv[1];

    if (strcmp(cmd, "sensor") == 0) {
        int fd = open_dev(TETHER_DEV_SENSOR, O_RDONLY);
        tether_hw_sensor s;
        if (fd < 0 || call(fd, DCMD_TETHER_READ_SENSOR, &s, sizeof(s)) != EOK) return 1;
        printf("paw=%.3f cmH2O flow=%.4f L/s seq=%llu\n", s.paw, s.flow, (unsigned long long)s.seq);
        return 0;
    }

    if (strcmp(cmd, "hw") == 0) {
        int fd = open_dev(TETHER_DEV_SENSOR, O_RDONLY);
        tether_hw_status st;
        if (fd < 0 || call(fd, DCMD_TETHER_HW_STATUS, &st, sizeof(st)) != EOK) return 1;
        printf("lung: R=%.1f C=%.3f | blower=%.2f cmH2O | commands=%llu | watchdog trips=%u active=%d | "
               "opens refused by OS=%u\n",
               st.lung.resistance, st.lung.compliance, st.u_applied, (unsigned long long)st.commands,
               st.watchdog_trips, st.watchdog_active, st.denied_opens);
        return 0;
    }

    if (strcmp(cmd, "actuate") == 0 && argc == 3) {
        int fd = open_dev(TETHER_DEV_ACTUATOR, O_WRONLY);
        if (fd < 0) return 1;
        tether_hw_command c = { atof(argv[2]), tether_now_ns() };
        return call(fd, DCMD_TETHER_SET_COMMAND, &c, sizeof(c)) == EOK ? 0 : 1;
    }

    if (strcmp(cmd, "lung") == 0 && argc == 3) {
        tether_lung l;
        if (tether_lung_preset(&l, argv[2]) != 0) {
            usage();
            return 2;
        }
        int fd = open_dev(TETHER_DEV_PLANT, O_WRONLY);
        if (fd < 0) return 1;
        return call(fd, DCMD_TETHER_SET_LUNG, &l, sizeof(l)) == EOK ? 0 : 1;
    }

    if (strcmp(cmd, "status") == 0) {
        tether_msg_hdr h = { TETHER_MSG_STATUS, 0, 0 };
        tether_status st;
        if (send_named(TETHER_ARBITER_NAME, &h, sizeof(h), &st, sizeof(st)) != (int)sizeof(st)) return 1;
        char buf[4096];
        if (tether_status_json(&st, buf, sizeof(buf)) < 0) return 1;
        puts(buf);
        return 0;
    }

    if (strcmp(cmd, "fault") == 0 && argc == 3) {
        tether_fault f = tether_fault_from_name(argv[2]);
        if (f == TETHER_FAULT_COUNT) {
            usage();
            return 2;
        }
        tether_msg_fault m = { { TETHER_MSG_FAULT, 0, 0 }, (int32_t)f, 0 };
        tether_fault_reply r;
        if (send_named(TETHER_AI_NAME, &m, sizeof(m), &r, sizeof(r)) != (int)sizeof(r)) return 1;
        printf("fault %s: %s\n", tether_fault_name((tether_fault)r.fault), r.status ? strerror(r.status) : "armed");
        return r.status ? 1 : 0;
    }

    usage();
    return 2;
}
