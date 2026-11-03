/*
 * Tether — the arbiter: safety monitor, switch and recovery controller.
 *
 * Roles follow ASTM F3269 run-time assurance:
 *   - Input manager:   tether_arbiter_step() receives the sensor reading and
 *                      the latest AI proposal with its timestamp.
 *   - Safety monitor:  tether_monitor_check() judges the proposal against the
 *                      envelope (freshness, range, rise, phase, model lookahead).
 *   - RTA switch:      AI <-> SAFE with hysteresis; the AI is readmitted only
 *                      at a breath boundary after N consecutive healthy proposals.
 *   - Recovery:        tether_recovery_command(), a plain pressure-control
 *                      breath whose bounds are verified statically at start-up.
 *
 * Everything here is pure computation: no I/O, no allocation, no blocking.
 * The QNX arbiter process calls tether_arbiter_step() from its timer pulse.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef TETHER_ARBITER_H
#define TETHER_ARBITER_H

#include <stdint.h>
#include "tether/common.h"
#include "tether/config.h"
#include "tether/estimator.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The AI's proposal as the arbiter last saw it. */
typedef struct {
    double u;            /* proposed airway pressure, cmH2O */
    uint32_t seq;        /* increments with every proposal */
    uint32_t model_id;   /* which model produced it */
    int64_t recv_ns;     /* when the arbiter received it */
    int valid;           /* 0 until the first proposal arrives */
} tether_proposal;

/* What the AI is told on every round trip (sent back in the MsgReply). */
typedef struct {
    uint64_t tick;
    int64_t t_ns;
    int32_t phase;        /* 0 = inspiration, 1 = expiration */
    int32_t mode;         /* tether_mode */
    double t_in_phase;    /* s since the phase began */
    double paw, flow, vol;
    double peep, ti, vt_target, rise_time;
    double vt_last;       /* tidal volume delivered in the previous breath, L */
    double dp_last;       /* peak applied pressure above PEEP in the previous breath */
    double est_c, est_r;  /* trusted estimate of lung compliance / resistance (0 until known) */
    uint32_t breath;
    uint32_t pad;
} tether_observation;

typedef struct {
    double u;             /* command written to the actuator */
    double u_ai;          /* AI proposal this tick (NaN if none) */
    int mode;             /* tether_mode actually used */
    int verdict;          /* tether_reason for this tick's proposal */
    int switched;         /* 1 if the mode changed on this tick */
    int violation;        /* 1 if the *measured* state broke the envelope */
} tether_decision;

typedef struct {
    int64_t t_ns;
    uint64_t tick;
    int32_t from_mode, to_mode;
    int32_t reason;
    uint32_t breath;
} tether_switch_event;

typedef struct {
    tether_config cfg;
    double dt;
    uint64_t cycle_ticks, insp_ticks;

    /* time and breath bookkeeping */
    uint64_t tick;
    uint32_t breath;
    int phase;
    uint64_t phase_tick;
    int64_t phase_ns;         /* wall time the current phase began */
    double vol;               /* volume delivered since the breath began, L */
    double paw, flow;         /* last sensor reading */
    double vol_peak, u_peak;  /* this breath */
    double vt_last, dp_last;  /* previous breath */
    int rec_cut;              /* recovery controller stopped inflating this breath */

    tether_estimator est;

    /* switch state */
    int mode;
    int reason;               /* why we are in SAFE (TETHER_OK when in AI) */
    int healthy_streak;
    double u_applied;         /* last command written */
    double u_ai_last_applied; /* last AI value we let through (for the rise check) */
    uint32_t last_seq;        /* last proposal seq seen by the rise check */

    /* counters */
    uint64_t ticks_ai, ticks_safe;
    uint64_t switches_to_safe, readmissions;
    uint64_t violations;
    uint64_t reason_count[TETHER_R_COUNT];
    tether_switch_event last_switch;
} tether_arbiter;

void tether_arbiter_init(tether_arbiter *a, const tether_config *cfg);

/* Verdict on a proposal given the arbiter's current (already updated) state.
 * ai_connected = 0 when the kernel has told us the AI process is gone. */
tether_reason tether_monitor_check(const tether_arbiter *a, const tether_proposal *p,
                                   int64_t now_ns, int ai_connected);

/* The verified fallback. Bounded by construction to [PEEP, PEEP + p_insp_safe]. */
double tether_recovery_command(const tether_arbiter *a);

/* One control tick. Updates breath state from the sensors, judges the
 * proposal, switches if needed, and returns the command to apply. */
tether_decision tether_arbiter_step(tether_arbiter *a, double paw, double flow,
                                    const tether_proposal *p, int64_t now_ns,
                                    int ai_connected);

void tether_arbiter_observe(const tether_arbiter *a, int64_t now_ns, tether_observation *out);

#ifdef __cplusplus
}
#endif
#endif
