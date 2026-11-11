/*
 * Tether — real-time plumbing shared by the QNX processes and the host sim:
 * latency histograms, a lock-free telemetry ring, a non-blocking proposal
 * mailbox, and the status snapshot that the dashboard renders.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef TETHER_RT_H
#define TETHER_RT_H

#include <stddef.h>
#include <stdint.h>
#include "tether/arbiter.h"

#ifdef __cplusplus
#include <atomic>
#define TETHER_ATOMIC(T) std::atomic<T>
extern "C" {
#else
#include <stdatomic.h>
#define TETHER_ATOMIC(T) _Atomic T
#endif

/* ---- latency histogram -------------------------------------------------
 * 1 us buckets below 1 ms, 10 us to 10 ms, 100 us to 100 ms, then overflow.
 * Fixed size, no allocation: safe to update from the control thread. */
#define TETHER_HIST_BUCKETS 2801
typedef struct {
    uint64_t count;
    int64_t min_ns, max_ns;
    double sum_ns;
    uint32_t bucket[TETHER_HIST_BUCKETS];
} tether_hist;

void tether_hist_reset(tether_hist *h);
void tether_hist_add(tether_hist *h, int64_t ns);
/* Upper edge of the bucket holding the p-th percentile (0 < p <= 100). */
int64_t tether_hist_percentile(const tether_hist *h, double p);

/* ---- telemetry ring ------------------------------------------------------
 * One writer (the control thread), any number of readers with their own
 * cursor. Readers never block the writer; a reader that falls behind skips. */
typedef struct {
    uint64_t tick;
    int64_t t_ns;
    float paw, flow, vol, u, u_ai;
    int32_t wake_ns;   /* how late this tick woke up */
    uint32_t breath;
    uint8_t mode, verdict, phase, violation;
} tether_sample;

#define TETHER_RING_CAP 8192
typedef struct {
    TETHER_ATOMIC(uint64_t) head;
    tether_sample s[TETHER_RING_CAP];
} tether_ring;

void tether_ring_init(tether_ring *r);
void tether_ring_push(tether_ring *r, const tether_sample *s);
/* Copies up to max samples starting at *cursor; advances *cursor. If the
 * cursor fell out of the window it is moved to the oldest valid sample. */
size_t tether_ring_read(tether_ring *r, uint64_t *cursor, tether_sample *out, size_t max);

/* ---- proposal mailbox ----------------------------------------------------
 * Seqlock written by the AI-facing IPC thread, read by the control thread.
 * The read gives up after a few attempts instead of spinning, so a writer
 * preempted mid-update can never stall the higher-priority reader. */
typedef struct {
    TETHER_ATOMIC(uint32_t) seq;
    tether_proposal p;
} tether_mailbox;

void tether_mailbox_init(tether_mailbox *m);
void tether_mailbox_write(tether_mailbox *m, const tether_proposal *p);
/* Returns 1 and fills *out on a consistent read, 0 otherwise. */
int tether_mailbox_read(tether_mailbox *m, tether_proposal *out);

/* ---- status snapshot -------------------------------------------------- */
#define TETHER_EVENT_LOG 16
typedef struct {
    uint64_t tick;
    int64_t t_ns;
    int32_t mode, reason;
    uint32_t breath;
    int32_t phase;
    double vt_last, dp_last, vt_target, vt_max, p_max, peep;
    double est_r, est_c;
    int32_t est_valid;
    int32_t ai_connected;
    uint32_t ai_model_id, ai_seq;
    int64_t ai_age_ns;
    uint64_t ticks_ai, ticks_safe, switches_to_safe, readmissions, violations;
    uint64_t reason_count[TETHER_R_COUNT];
    /* scheduling latency of the control loop (timer expiry -> thread running) */
    uint64_t wake_count;
    int64_t wake_p50_ns, wake_p99_ns, wake_p999_ns, wake_max_ns;
    int64_t exec_p99_ns, exec_max_ns;
    uint32_t ai_restarts, hw_watchdog_trips, hw_denied_opens;
    uint32_t guard_off; /* 1 while the arbiter is in demo pass-through (no runtime assurance) */
    uint32_t pad2;
    uint64_t ctl_overruns; /* control ticks skipped because the loop fell more than 10 periods behind */
    uint64_t event_seq; /* total switch events so far */
    tether_switch_event events[TETHER_EVENT_LOG]; /* events[(event_seq-1-i) % N] newest first */
} tether_status;

typedef struct {
    uint64_t seq;
    tether_switch_event ev[TETHER_EVENT_LOG];
} tether_eventlog;

void tether_eventlog_push(tether_eventlog *l, const tether_switch_event *e);

void tether_status_fill(tether_status *s, const tether_arbiter *a, const tether_hist *wake,
                        const tether_hist *exec, const tether_eventlog *log,
                        const tether_proposal *last, int64_t now_ns, int ai_connected);

/* JSON for the dashboard. Return bytes written (excluding NUL), or -1 if
 * the buffer was too small. */
int tether_status_json(const tether_status *s, char *buf, size_t len);
int tether_samples_json(const tether_sample *s, size_t n, size_t stride, char *buf, size_t len);

#ifdef __cplusplus
}
#endif
#endif
