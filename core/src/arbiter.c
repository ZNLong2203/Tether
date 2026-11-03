/* SPDX-License-Identifier: Apache-2.0 */
#include "tether/arbiter.h"

#include <math.h>
#include <string.h>

static double clampd(double x, double lo, double hi)
{
    return x < lo ? lo : (x > hi ? hi : x);
}

void tether_arbiter_init(tether_arbiter *a, const tether_config *cfg)
{
    memset(a, 0, sizeof(*a));
    a->cfg = *cfg;
    a->dt = 1.0 / cfg->timing.control_hz;
    a->cycle_ticks = (uint64_t)llround(60.0 / cfg->vent.rr * cfg->timing.control_hz);
    a->insp_ticks = (uint64_t)llround(cfg->vent.ti * cfg->timing.control_hz);
    a->mode = TETHER_MODE_SAFE;
    a->reason = TETHER_R_STARTUP;
    a->u_applied = cfg->vent.peep;
    a->u_ai_last_applied = cfg->vent.peep;
    a->phase = 0;
    a->u_peak = cfg->vent.peep;
    tether_est_reset(&a->est);
    tether_est_begin_breath(&a->est);
}

tether_reason tether_monitor_check(const tether_arbiter *a, const tether_proposal *p,
                                   int64_t now_ns, int ai_connected)
{
    const tether_envelope *e = &a->cfg.env;
    const tether_vent_settings *v = &a->cfg.vent;

    if (p == NULL || !p->valid) return TETHER_R_NO_PROPOSAL;
    if (!ai_connected) return TETHER_R_DISCONNECTED;
    if ((double)(now_ns - p->recv_ns) > e->max_age_s * 1e9) return TETHER_R_STALE;
    if (!isfinite(p->u)) return TETHER_R_NONFINITE;
    if (p->u < e->p_min || p->u > e->p_max) return TETHER_R_RANGE;
    if (a->est.valid == 0) return TETHER_R_STARTUP;

    if (a->phase == 1) {
        /* The AI works from an observation up to one round trip old, so for
         * max_age_s after the phase flips its proposal may still describe the
         * previous phase. Such a proposal is not a fault; the arbiter simply
         * does not apply it (see tether_arbiter_step). */
        if (now_ns - a->phase_ns <= (int64_t)(e->max_age_s * 1e9)) return TETHER_OK;
        if (p->u > v->peep + e->exp_tolerance) return TETHER_R_EXP_PHASE;
        return TETHER_OK;
    }

    /* Inspiration. A new proposal may not raise pressure by more than
     * rise_step_max over what is being applied right now. */
    if (p->u - a->u_applied > e->rise_step_max) return TETHER_R_RISE;

    if (a->vol >= e->vt_max * 0.98 && p->u > v->peep + e->exp_tolerance)
        return TETHER_R_VOLUME_LIMIT;

    /* Model lookahead with conservative mechanics: what volume would the lung
     * reach by the end of this inspiration if the command were held? */
    double c = clampd(a->est.c * e->c_margin, e->c_min, e->c_max);
    double r = clampd(a->est.r * e->r_margin, e->r_min, e->r_max);
    uint64_t elapsed = a->tick - a->phase_tick;
    double remaining = (a->insp_ticks > elapsed ? (double)(a->insp_ticks - elapsed) : 0.0) * a->dt;
    double v_ss = c * (p->u - v->peep);
    double v_pred = v_ss + (a->vol - v_ss) * exp(-remaining / (r * c));
    if (v_pred > e->vt_max) return TETHER_R_PREDICT_VOL;

    return TETHER_OK;
}

double tether_recovery_command(const tether_arbiter *a)
{
    const tether_vent_settings *v = &a->cfg.vent;
    if (a->phase == 1 || a->rec_cut) return v->peep;
    double t = (double)(a->tick - a->phase_tick) * a->dt;
    double ramp = t >= v->rise_time ? 1.0 : t / v->rise_time;
    return v->peep + a->cfg.rec.p_insp_safe * ramp;
}

static void record_switch(tether_arbiter *a, int64_t now_ns, int from, int to, int reason)
{
    a->last_switch.t_ns = now_ns;
    a->last_switch.tick = a->tick;
    a->last_switch.from_mode = from;
    a->last_switch.to_mode = to;
    a->last_switch.reason = reason;
    a->last_switch.breath = a->breath;
}

/* Breath clock: time-cycled, owned by the arbiter. */
static int update_phase(tether_arbiter *a, int64_t now_ns)
{
    uint64_t pos = a->tick % a->cycle_ticks;
    int breath_start = 0;
    if (pos == 0) {
        if (a->tick > 0) {
            a->vt_last = a->vol_peak;
            a->dp_last = a->u_peak - a->cfg.vent.peep;
            a->breath++;
        }
        a->vol = 0.0;
        a->vol_peak = 0.0;
        a->u_peak = a->cfg.vent.peep;
        a->rec_cut = 0;
        a->phase = 0;
        a->phase_tick = a->tick;
        a->phase_ns = now_ns;
        tether_est_begin_breath(&a->est);
        breath_start = 1;
    } else if (pos == a->insp_ticks) {
        const tether_envelope *e = &a->cfg.env;
        tether_est_end_breath(&a->est, e->c_min, e->c_max, e->r_min, e->r_max);
        a->phase = 1;
        a->phase_tick = a->tick;
        a->phase_ns = now_ns;
    }
    return breath_start;
}

tether_decision tether_arbiter_step(tether_arbiter *a, double paw, double flow,
                                    const tether_proposal *p, int64_t now_ns, int ai_connected)
{
    const tether_config *cfg = &a->cfg;
    tether_decision d;
    memset(&d, 0, sizeof(d));

    int breath_start = update_phase(a, now_ns);

    /* Trusted sensing: integrate volume and feed the mechanics estimator. */
    a->paw = paw;
    a->flow = flow;
    if (isfinite(flow)) a->vol += flow * a->dt;
    if (a->vol > a->vol_peak) a->vol_peak = a->vol;
    if (a->phase == 0 && isfinite(paw) && isfinite(flow))
        tether_est_add(&a->est, paw - cfg->vent.peep, flow, a->vol);
    if (a->phase == 0 && a->vol >= cfg->rec.vol_cutoff * cfg->env.vt_max) a->rec_cut = 1;

    /* Outcome check on the measured patient state, independent of who drives. */
    int measured = TETHER_OK;
    if (paw > cfg->env.p_max + 0.5) measured = TETHER_R_MEASURED_PRESSURE;
    else if (a->vol > cfg->env.vt_max + 0.01) measured = TETHER_R_VOLUME_LIMIT;
    if (measured != TETHER_OK) {
        d.violation = 1;
        a->violations++;
    }

    tether_reason verdict = tether_monitor_check(a, p, now_ns, ai_connected);
    if (measured != TETHER_OK) verdict = (tether_reason)measured;
    d.verdict = verdict;
    d.u_ai = (p != NULL && p->valid) ? p->u : NAN;

    if (a->mode == TETHER_MODE_AI) {
        if (verdict != TETHER_OK) {
            record_switch(a, now_ns, TETHER_MODE_AI, TETHER_MODE_SAFE, verdict);
            a->mode = TETHER_MODE_SAFE;
            a->reason = verdict;
            a->healthy_streak = 0;
            a->switches_to_safe++;
            a->reason_count[verdict]++;
            d.switched = 1;
        }
    } else {
        if (verdict == TETHER_OK) {
            a->healthy_streak++;
        } else {
            a->healthy_streak = 0;
            a->reason = verdict;
        }
        /* Readmit only at a breath boundary, so a breath is never split
         * between two controllers on the way back. */
        if (breath_start && a->healthy_streak >= cfg->env.readmit_ticks) {
            record_switch(a, now_ns, TETHER_MODE_SAFE, TETHER_MODE_AI, TETHER_OK);
            a->mode = TETHER_MODE_AI;
            a->reason = TETHER_OK;
            a->readmissions++;
            d.switched = 1;
        }
    }

    double u;
    if (a->mode == TETHER_MODE_AI) {
        u = p->u;
        if (a->phase == 1 && now_ns - a->phase_ns <= (int64_t)(cfg->env.max_age_s * 1e9))
            u = cfg->vent.peep; /* ride out a proposal from the previous phase */
        a->u_ai_last_applied = u;
        a->ticks_ai++;
    } else {
        u = tether_recovery_command(a);
        a->ticks_safe++;
    }
    if (p != NULL && p->valid) a->last_seq = p->seq;

    if (a->phase == 0 && u > a->u_peak) a->u_peak = u;
    a->u_applied = u;
    d.u = u;
    d.mode = a->mode;
    a->tick++;
    return d;
}

void tether_arbiter_observe(const tether_arbiter *a, int64_t now_ns, tether_observation *o)
{
    memset(o, 0, sizeof(*o));
    o->tick = a->tick;
    o->t_ns = now_ns;
    o->phase = a->phase;
    o->mode = a->mode;
    o->t_in_phase = (double)(a->tick - a->phase_tick) * a->dt;
    o->paw = a->paw;
    o->flow = a->flow;
    o->vol = a->vol;
    o->peep = a->cfg.vent.peep;
    o->ti = a->cfg.vent.ti;
    o->vt_target = a->cfg.vent.vt_target;
    o->rise_time = a->cfg.vent.rise_time;
    o->vt_last = a->vt_last;
    o->dp_last = a->dp_last;
    o->est_c = a->est.valid ? a->est.c : 0.0;
    o->est_r = a->est.valid ? a->est.r : 0.0;
    o->breath = a->breath;
}
