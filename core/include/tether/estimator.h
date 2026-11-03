/*
 * Tether — trusted estimate of the patient's lung mechanics.
 *
 * Least-squares fit of the equation of motion over each inspiration:
 *     P_aw - PEEP = R * Q + V / C
 * The monitor uses this to predict what an AI command would do to the lung.
 * It is fed only from the sensors, never from the AI, so a misbehaving model
 * cannot talk the monitor into a looser envelope.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef TETHER_ESTIMATOR_H
#define TETHER_ESTIMATOR_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    /* running sums for the current inspiration */
    double sqq, sqv, svv, spq, spv;
    int n;
    /* last accepted estimate */
    double r, c;
    int valid;   /* number of breaths that produced a usable estimate */
} tether_estimator;

void tether_est_reset(tether_estimator *e);
void tether_est_begin_breath(tether_estimator *e);
void tether_est_add(tether_estimator *e, double p_minus_peep, double flow, double vol);
/* Solve for R and C; keeps the previous estimate if the fit is ill-posed or
 * lands outside [c_min,c_max] x [r_min,r_max]. Returns 1 if updated. */
int tether_est_end_breath(tether_estimator *e, double c_min, double c_max, double r_min, double r_max);

#ifdef __cplusplus
}
#endif
#endif
