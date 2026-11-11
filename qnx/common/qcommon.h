/*
 * Tether on QNX — small helpers shared by the processes: logging (stderr and
 * slog2), command-line configuration, real-time setup, and the seqlock
 * snapshot used to hand state from the control thread to other threads.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef TETHER_QCOMMON_H
#define TETHER_QCOMMON_H

#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>
#include <string.h>
#include "tether/config.h"

/* Thread priorities (SCHED_FIFO). The control loop outranks everything the
 * AI can reach; the AI outranks only the dashboard. */
#define TETHER_PRIO_CONTROL 60
#define TETHER_PRIO_HW_RESMGR 58
#define TETHER_PRIO_HW_PHYSICS 56
#define TETHER_PRIO_ARB_IPC 50
#define TETHER_PRIO_SUPERVISOR 40
#define TETHER_PRIO_AI 30
#define TETHER_PRIO_BRIDGE 20

/* Logs one line to stderr and to the "tether" slog2 buffer set. */
void qlog(const char *tag, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

/* Consumes one --key=value option that changes the shared configuration
 * (--control-hz, --ai-hz, --plant-hz, --max-age-ms, --readmit-ticks,
 * --hw-watchdog-ms). Returns 1 if it was one of those, 0 if not. */
int q_config_arg(const char *arg, tether_config *cfg);
/* Defaults, then every shared option found in argv, then validation.
 * Returns 0 or -1 (message already logged). */
int q_config_load(int argc, char **argv, tether_config *cfg, const char *tag);

/* Puts the calling thread under SCHED_FIFO at prio. Returns 0 or an errno. */
int q_set_fifo(int prio);
/* Locks all current and future pages in RAM. Returns 0 or errno. */
int q_lock_memory(void);
/* Sets the system clock tick (QNX default is 1 ms). Returns 0 or errno. */
int q_clock_period_us(unsigned us);

/* ---- seqlock snapshot: one writer, any number of readers ---------------
 * The writer never waits. A reader copies, then checks the sequence did not
 * move; it gives up after a bounded number of tries instead of spinning. */
#define Q_SNAPSHOT(NAME, T) \
    typedef struct { _Atomic uint32_t seq; T v; } NAME

static inline void q_snap_write_raw(_Atomic uint32_t *seq, void *dst, const void *src, size_t n)
{
    uint32_t s = atomic_load_explicit(seq, memory_order_relaxed);
    atomic_store_explicit(seq, s + 1, memory_order_relaxed);
    atomic_thread_fence(memory_order_release);
    memcpy(dst, src, n);
    atomic_thread_fence(memory_order_release);
    atomic_store_explicit(seq, s + 2, memory_order_release);
}

static inline int q_snap_read_raw(_Atomic uint32_t *seq, const void *src, void *dst, size_t n)
{
    for (int tries = 0; tries < 8; tries++) {
        uint32_t a = atomic_load_explicit(seq, memory_order_acquire);
        if (a & 1u) continue;
        memcpy(dst, src, n);
        atomic_thread_fence(memory_order_acquire);
        if (atomic_load_explicit(seq, memory_order_relaxed) == a) return 1;
    }
    return 0;
}

#define q_snap_write(S, SRC) q_snap_write_raw(&(S)->seq, &(S)->v, (SRC), sizeof((S)->v))
#define q_snap_read(S, DST) q_snap_read_raw(&(S)->seq, &(S)->v, (DST), sizeof((S)->v))

#endif
