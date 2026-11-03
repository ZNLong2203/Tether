/*
 * Tether — simulated patient and ventilator hardware.
 *
 * Single-compartment lung (the standard "equation of motion" of respiratory
 * mechanics) driven by a pressure source with a first-order lag:
 *
 *     P_aw  = P_blower
 *     Q     = (P_aw - V/C + P_mus) / R          flow into the lung
 *     dV/dt = Q
 *     dP_blower/dt = (u - P_blower) / tau        actuator lag
 *
 * V is lung volume above the relaxed volume, so at PEEP the lung holds C*PEEP.
 * P_mus >= 0 is the patient's own inspiratory effort (0 = sedated patient).
 *
 * This is the only simulated part of Tether. On QNX it sits behind a real
 * resource manager (/dev/tether/{actuator,sensor,plant}), the same interface
 * a driver for a physical blower and flow sensor would offer.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef TETHER_PLANT_H
#define TETHER_PLANT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    double resistance;  /* cmH2O/(L/s); healthy ~5-10, bronchospasm 20+ */
    double compliance;  /* L/cmH2O;    healthy ~0.05, ARDS 0.02-0.03 */
    double blower_tau;  /* s */
    double effort_amp;  /* cmH2O, peak patient effort; 0 = passive */
    double effort_rr;   /* patient's own breathing rate, breaths/min */
    double noise;       /* sensor noise standard deviation (pressure units) */
} tether_lung;

typedef struct {
    double t;      /* simulated time, s */
    double v;      /* lung volume above relaxed volume, L */
    double pb;     /* blower (= airway) pressure, cmH2O */
    double q;      /* flow, L/s (+ into the patient) */
    double pmus;   /* current patient effort, cmH2O */
    uint64_t rng;  /* xorshift state for sensor noise */
} tether_plant;

typedef struct {
    double paw;  /* measured airway pressure, cmH2O */
    double flow; /* measured flow, L/s */
} tether_sensor_reading;

void tether_lung_defaults(tether_lung *lung);

/* Start at equilibrium with the given PEEP applied. */
void tether_plant_init(tether_plant *p, const tether_lung *lung, double peep, uint64_t seed);

/* Advance the physics by dt seconds holding command u (cmH2O), using fixed
 * sub-steps no longer than 0.1 ms. */
void tether_plant_step(tether_plant *p, const tether_lung *lung, double u, double dt);

/* Read the sensors (adds noise if lung->noise > 0). */
tether_sensor_reading tether_plant_read(tether_plant *p, const tether_lung *lung);

#ifdef __cplusplus
}
#endif
#endif
