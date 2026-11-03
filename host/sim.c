/* SPDX-License-Identifier: Apache-2.0 */
#include "sim.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

void sim_defaults(sim_scenario *s, const tether_config *cfg, const tether_mlp *model)
{
    memset(s, 0, sizeof(*s));
    s->cfg = *cfg;
    tether_lung_defaults(&s->lung);
    s->lung2 = s->lung;
    s->model = model;
    s->ctrl = SIM_CTRL_AI;
    s->fault = TETHER_FAULT_NONE;
    s->duration_s = 60.0;
    s->seed = 1;
}

/* How long the supervisor takes to bring a crashed AI process back. */
#define AI_RESTART_S 0.5
/* How long a hung AI goes unnoticed before the supervisor kills it. */
#define HANG_KILL_S 1.0

void sim_run(const sim_scenario *sc, sim_result *out)
{
    const tether_config *cfg = &sc->cfg;
    memset(out, 0, sizeof(*out));
    out->first_switch_latency_s = -1.0;
    out->min_paw = 1e9;
    out->vt_min_after_fault = 1e9;

    tether_arbiter a;
    tether_arbiter_init(&a, cfg);

    tether_lung lung = sc->lung;
    tether_plant plant;
    tether_plant_init(&plant, &lung, cfg->vent.peep, sc->seed);

    const double dt = 1.0 / cfg->timing.control_hz;
    const uint64_t n_ticks = (uint64_t)(sc->duration_s * cfg->timing.control_hz);
    const uint64_t ai_every = (uint64_t)llround(cfg->timing.control_hz / cfg->timing.ai_hz);
    const uint64_t slow_every = (uint64_t)llround(0.040 * cfg->timing.control_hz);

    if (sc->record) {
        out->samples = calloc(n_ticks, sizeof(tether_sample));
    }

    tether_fault_state fs;
    tether_fault_arm(&fs, TETHER_FAULT_NONE, 0);
    int fault_on = 0;
    uint32_t fault_breath = 0;

    tether_proposal prop;
    memset(&prop, 0, sizeof(prop));
    int ai_alive = 1;
    int ai_connected = 1;
    int64_t ai_down_until_ns = 0;
    int64_t hang_kill_ns = -1;
    int process_fault_done = 0;
    uint32_t seq = 0;
    double raw_u = cfg->vent.peep; /* what a raw actuator holds */
    uint64_t next_ai_tick = 0;
    double sum_vt = 0, sum_err = 0;
    int n_vt = 0;
    uint32_t last_breath_counted = 0;
    int64_t fault_start_ns = (int64_t)(sc->fault_at_s * 1e9);
    int switched_after_fault = 0;
    tether_sensor_reading rd;
    double u_cmd = cfg->vent.peep;
    int lung_changed = 0;

    for (uint64_t k = 0; k < n_ticks; k++) {
        double t = (double)k * dt;
        int64_t now_ns = (int64_t)(t * 1e9);

        if (!lung_changed && sc->lung_change_s > 0 && t >= sc->lung_change_s) {
            lung = sc->lung2;
            lung_changed = 1;
        }

        /* fault window */
        int want_fault = sc->fault != TETHER_FAULT_NONE && t >= sc->fault_at_s &&
                         (sc->fault_until_s <= sc->fault_at_s || t < sc->fault_until_s);
        int process_fault = sc->fault == TETHER_FAULT_CRASH || sc->fault == TETHER_FAULT_HANG;
        if (want_fault && !fault_on && !(process_fault && process_fault_done)) {
            fault_on = 1;
            fault_breath = a.breath;
            tether_fault_arm(&fs, sc->fault, fault_breath);
            if (sc->fault == TETHER_FAULT_CRASH) {
                /* the kernel tells the arbiter at once (disconnect pulse) */
                ai_alive = 0;
                ai_connected = 0;
                ai_down_until_ns = now_ns + (int64_t)(AI_RESTART_S * 1e9);
            } else if (sc->fault == TETHER_FAULT_HANG) {
                /* still connected, but silent until the supervisor notices
                 * the missing heartbeat and kills it */
                ai_alive = 0;
                hang_kill_ns = now_ns + (int64_t)(HANG_KILL_S * 1e9);
            }
        } else if (!want_fault && fault_on) {
            fault_on = 0;
            tether_fault_arm(&fs, TETHER_FAULT_NONE, 0);
        }
        if (sc->fault == TETHER_FAULT_HANG && hang_kill_ns >= 0 && now_ns >= hang_kill_ns) {
            hang_kill_ns = -1;
            ai_connected = 0;
            ai_down_until_ns = now_ns + (int64_t)(AI_RESTART_S * 1e9);
        }
        if (process_fault && !ai_alive && ai_down_until_ns > 0 && now_ns >= ai_down_until_ns) {
            /* a restarted process comes back healthy and reconnects */
            ai_alive = 1;
            ai_connected = 1;
            ai_down_until_ns = 0;
            process_fault_done = 1;
            fault_on = 0;
        }

        /* sensors */
        rd = tether_plant_read(&plant, &lung);

        /* AI inference on its own clock */
        int slow = fault_on && sc->fault == TETHER_FAULT_SLOW;
        if (sc->ctrl != SIM_CTRL_RECOVERY && ai_alive && k >= next_ai_tick) {
            next_ai_tick = k + (slow ? slow_every : ai_every);
            tether_observation obs;
            tether_arbiter_observe(&a, now_ns, &obs);
            double u;
            if (sc->ctrl == SIM_CTRL_TEACHER)
                u = tether_teacher_command(&obs, lung.resistance, lung.compliance);
            else
                u = tether_policy_command(sc->model, &obs);
            if (sc->ctrl != SIM_CTRL_TEACHER && fault_on) u = tether_fault_apply(&fs, &obs, u);
            prop.u = u;
            prop.seq = ++seq;
            prop.model_id = sc->model ? sc->model->model_id : 0;
            prop.recv_ns = now_ns;
            prop.valid = 1;
        }

        const tether_proposal *pp = (sc->ctrl == SIM_CTRL_RECOVERY) ? NULL : &prop;
        tether_decision d = tether_arbiter_step(&a, rd.paw, rd.flow, pp, now_ns, ai_connected);

        if (sc->ctrl == SIM_CTRL_AI_RAW || sc->ctrl == SIM_CTRL_TEACHER) {
            /* No Tether: whatever the controller last said goes to the blower. */
            if (prop.valid && isfinite(prop.u)) raw_u = prop.u;
            u_cmd = raw_u;
        } else {
            u_cmd = d.u;
        }

        if (d.switched && d.mode == TETHER_MODE_SAFE && fault_on && !switched_after_fault &&
            out->first_switch_latency_s < 0) {
            out->first_switch_latency_s = (double)(now_ns - fault_start_ns) / 1e9;
            switched_after_fault = 1;
        }

        tether_plant_step(&plant, &lung, u_cmd, dt);

        /* outcome metrics, independent of the arbiter's own bookkeeping */
        double vol_above = plant.v - lung.compliance * cfg->vent.peep;
        if (plant.pb > out->max_paw) out->max_paw = plant.pb;
        if (plant.pb < out->min_paw) out->min_paw = plant.pb;
        if (vol_above > out->max_vol) out->max_vol = vol_above;
        if (plant.pb > cfg->env.p_max) out->time_over_pmax_s += dt;
        if (vol_above > cfg->env.vt_max) out->time_over_vtmax_s += dt;
        if (d.violation) out->violation_ticks++;

        if (a.breath != last_breath_counted) {
            last_breath_counted = a.breath;
            if (sc->fault != TETHER_FAULT_NONE && t > sc->fault_at_s + 1e-9 && a.vt_last < out->vt_min_after_fault)
                out->vt_min_after_fault = a.vt_last;
            if (t > 0.6 * sc->duration_s && a.vt_last > 0) {
                sum_vt += a.vt_last;
                sum_err += fabs(a.vt_last - cfg->vent.vt_target) / cfg->vent.vt_target;
                n_vt++;
            }
        }

        if (out->samples) {
            tether_sample *s = &out->samples[out->n_samples++];
            s->tick = k;
            s->t_ns = now_ns;
            s->paw = (float)plant.pb;
            s->flow = (float)plant.q;
            s->vol = (float)vol_above;
            s->u = (float)u_cmd;
            s->u_ai = (float)d.u_ai;
            s->breath = a.breath;
            s->mode = (uint8_t)d.mode;
            s->verdict = (uint8_t)d.verdict;
            s->phase = (uint8_t)a.phase;
            s->violation = (uint8_t)d.violation;
        }
    }

    if (out->vt_min_after_fault > 1e8) out->vt_min_after_fault = 0.0;
    out->switches_to_safe = a.switches_to_safe;
    out->readmissions = a.readmissions;
    out->breaths = (int)a.breath;
    out->final_mode = a.mode;
    memcpy(out->reason_count, a.reason_count, sizeof(out->reason_count));
    if (n_vt > 0) {
        out->vt_mean_after = sum_vt / n_vt;
        out->vt_err_mean_after = sum_err / n_vt;
    }
}

void sim_free(sim_result *r)
{
    free(r->samples);
    r->samples = NULL;
    r->n_samples = 0;
}
