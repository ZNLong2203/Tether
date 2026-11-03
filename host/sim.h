/*
 * Closed-loop simulation of Tether in simulated time. It drives exactly the
 * code the QNX arbiter runs (tether_arbiter_step), against the simulated
 * lung, with a software model of each fault. It is what lets the safety
 * envelope be tested exhaustively on a laptop; the QNX build then shows the
 * same behaviour with a real kernel, scheduler and process isolation.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef TETHER_SIM_H
#define TETHER_SIM_H

#include "tether/arbiter.h"
#include "tether/plant.h"
#include "tether/policy.h"
#include "tether/rt.h"

typedef enum {
    SIM_CTRL_AI,       /* the MLP, behind the arbiter */
    SIM_CTRL_AI_RAW,   /* the MLP wired straight to the actuator (no Tether) */
    SIM_CTRL_RECOVERY, /* the fixed-pressure fallback alone */
    SIM_CTRL_TEACHER   /* privileged oracle, for reference */
} sim_controller;

typedef struct {
    tether_config cfg;
    tether_lung lung;
    const tether_mlp *model;
    sim_controller ctrl;
    tether_fault fault;
    double fault_at_s;      /* when the fault starts */
    double fault_until_s;   /* when it clears (<= fault_at_s: never) */
    double duration_s;
    /* lung change: at lung_change_s the lung switches to lung2 */
    double lung_change_s;
    tether_lung lung2;
    uint64_t seed;
    int record;             /* keep per-tick samples */
} sim_scenario;

typedef struct {
    double max_paw, max_vol, min_paw;
    double time_over_pmax_s, time_over_vtmax_s;
    uint64_t violation_ticks;
    uint64_t switches_to_safe, readmissions;
    double first_switch_latency_s; /* fault start -> first switch, -1 if none */
    double vt_mean_after, vt_err_mean_after;   /* breaths in the last 40% of the run */
    int breaths;
    double vt_min_after_fault; /* smallest tidal volume of any breath completed after fault onset */
    uint64_t reason_count[TETHER_R_COUNT];
    int final_mode;
    tether_sample *samples;
    size_t n_samples;
} sim_result;

void sim_defaults(sim_scenario *s, const tether_config *cfg, const tether_mlp *model);
void sim_run(const sim_scenario *s, sim_result *out);
void sim_free(sim_result *r);

#endif
