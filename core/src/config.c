/* SPDX-License-Identifier: Apache-2.0 */
#include "tether/config.h"

#include <stdarg.h>
#include <stdio.h>

void tether_config_defaults(tether_config *cfg)
{
    /* Lung-protective ventilation for a 70 kg predicted body weight adult:
     * 6 mL/kg target, 8 mL/kg hard ceiling, 35 cmH2O pressure ceiling. */
    cfg->pbw_kg = 70.0;

    cfg->vent.rr = 15.0;
    cfg->vent.ti = 1.0;
    cfg->vent.peep = 5.0;
    cfg->vent.vt_target = 0.006 * cfg->pbw_kg; /* 0.42 L */
    cfg->vent.rise_time = 0.10;

    cfg->env.p_max = 35.0;
    cfg->env.p_min = 3.0;
    cfg->env.vt_max = 0.008 * cfg->pbw_kg; /* 0.56 L */
    cfg->env.rise_step_max = 6.0;
    cfg->env.exp_tolerance = 1.0;
    cfg->env.max_age_s = 0.025;
    cfg->env.c_min = 0.010;
    cfg->env.c_max = 0.060;
    cfg->env.r_min = 3.0;
    cfg->env.r_max = 40.0;
    cfg->env.c_margin = 1.10;
    cfg->env.r_margin = 0.80;
    cfg->env.readmit_ticks = 200;

    cfg->rec.p_insp_safe = 9.0;
    cfg->rec.vol_cutoff = 0.90;

    cfg->timing.control_hz = 1000.0;
    cfg->timing.ai_hz = 100.0;
    cfg->timing.plant_hz = 2000.0;
    cfg->timing.hw_watchdog_s = 0.050;
}

static int fail(char *err, size_t errlen, const char *fmt, ...)
{
    if (err != NULL && errlen > 0) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(err, errlen, fmt, ap);
        va_end(ap);
    }
    return -1;
}

int tether_config_validate(const tether_config *c, char *err, size_t errlen)
{
    const tether_vent_settings *v = &c->vent;
    const tether_envelope *e = &c->env;
    const tether_recovery *r = &c->rec;
    const tether_timing *t = &c->timing;

    if (!(v->rr > 0 && v->rr <= 60)) return fail(err, errlen, "rr must be in (0, 60]");
    if (!(v->ti > 0 && v->ti < 60.0 / v->rr)) return fail(err, errlen, "ti must be shorter than the breath");
    if (!(v->rise_time > 0 && v->rise_time < v->ti)) return fail(err, errlen, "rise_time must be in (0, ti)");
    if (!(v->vt_target > 0 && v->vt_target < e->vt_max))
        return fail(err, errlen, "vt_target must be below vt_max");
    if (!(e->p_min <= v->peep && v->peep < e->p_max))
        return fail(err, errlen, "peep must lie in [p_min, p_max)");
    if (!(e->c_min > 0 && e->c_min < e->c_max)) return fail(err, errlen, "need 0 < c_min < c_max");
    if (!(e->r_min > 0 && e->r_min < e->r_max)) return fail(err, errlen, "need 0 < r_min < r_max");
    if (!(e->c_margin >= 1.0)) return fail(err, errlen, "c_margin must be >= 1 (conservative)");
    if (!(e->r_margin > 0 && e->r_margin <= 1.0)) return fail(err, errlen, "r_margin must be in (0, 1]");
    if (!(t->control_hz > 0 && t->ai_hz > 0 && t->ai_hz <= t->control_hz))
        return fail(err, errlen, "need 0 < ai_hz <= control_hz");
    if (!(t->plant_hz >= t->control_hz)) return fail(err, errlen, "plant_hz must be >= control_hz");
    if (!(e->max_age_s >= 1.0 / t->ai_hz))
        return fail(err, errlen, "max_age_s shorter than one AI period: every proposal would be stale");
    if (!(e->readmit_ticks > 0)) return fail(err, errlen, "readmit_ticks must be positive");

    /* Static verification of the recovery controller against the envelope.
     * Pressure: its output is bounded by [PEEP, PEEP + p_insp_safe]. */
    if (!(r->p_insp_safe > 0 && v->peep + r->p_insp_safe <= e->p_max))
        return fail(err, errlen, "recovery pressure PEEP + p_insp_safe exceeds p_max");
    /* Volume: holding PEEP + p_insp_safe forever on the most compliant lung
     * the envelope admits delivers c_max * p_insp_safe. That must fit. */
    if (!(e->c_max * r->p_insp_safe <= e->vt_max))
        return fail(err, errlen,
                    "recovery controller could deliver %.3f L on a c_max lung, above vt_max %.3f L",
                    e->c_max * r->p_insp_safe, e->vt_max);
    if (!(r->vol_cutoff > 0 && r->vol_cutoff <= 1.0)) return fail(err, errlen, "vol_cutoff must be in (0, 1]");
    if (!(t->hw_watchdog_s > 1.0 / t->control_hz))
        return fail(err, errlen, "hardware watchdog must be longer than one control period");
    return 0;
}
