/*
 * Tether — configuration: ventilator settings, the safety envelope, and
 * timing. One struct, validated once at start-up; a configuration that
 * would let the recovery controller itself break the envelope is refused.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef TETHER_CONFIG_H
#define TETHER_CONFIG_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    double rr;          /* breaths per minute */
    double ti;          /* inspiratory time, s */
    double peep;        /* positive end-expiratory pressure, cmH2O */
    double vt_target;   /* tidal volume target, L (6 mL/kg predicted body weight) */
    double rise_time;   /* pressure ramp at the start of inspiration, s */
} tether_vent_settings;

typedef struct {
    double p_max;         /* airway pressure ceiling, cmH2O */
    double p_min;         /* airway pressure floor, cmH2O */
    double vt_max;        /* delivered volume ceiling, L (8 mL/kg PBW) */
    double rise_step_max; /* largest pressure increase between two AI updates, cmH2O */
    double exp_tolerance; /* allowed pressure above PEEP during expiration, cmH2O */
    double max_age_s;     /* freshness deadline for an AI proposal, s */
    double c_min, c_max;  /* plausible compliance range, L/cmH2O */
    double r_min, r_max;  /* plausible resistance range, cmH2O/(L/s) */
    double c_margin;      /* multiply estimated compliance by this (>1) when predicting */
    double r_margin;      /* multiply estimated resistance by this (<1) when predicting */
    int readmit_ticks;    /* consecutive healthy proposals before the AI is readmitted */
} tether_envelope;

typedef struct {
    double p_insp_safe; /* recovery controller: inspiratory pressure above PEEP, cmH2O */
    double vol_cutoff;  /* recovery controller: stop inflating at this fraction of vt_max */
} tether_recovery;

typedef struct {
    double control_hz;  /* arbiter tick rate */
    double ai_hz;       /* AI inference rate */
    double plant_hz;    /* virtual hardware integration rate */
    double hw_watchdog_s; /* virtual hardware fails safe if no command for this long */
} tether_timing;

typedef struct {
    tether_vent_settings vent;
    tether_envelope env;
    tether_recovery rec;
    tether_timing timing;
    double pbw_kg; /* predicted body weight, documentation only */
} tether_config;

/* Defaults for a 70 kg predicted-body-weight adult (lung-protective settings). */
void tether_config_defaults(tether_config *cfg);

/* Returns 0 if the configuration is coherent. Otherwise returns -1 and writes
 * a human-readable reason into err (if err != NULL). The key check is static
 * verification of the recovery controller: even with the most compliant lung
 * the envelope allows (c_max), its inspiratory pressure held forever must not
 * be able to deliver more than vt_max, and it must stay inside [p_min, p_max]. */
int tether_config_validate(const tether_config *cfg, char *err, size_t errlen);

#ifdef __cplusplus
}
#endif
#endif
