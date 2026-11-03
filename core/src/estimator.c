/* SPDX-License-Identifier: Apache-2.0 */
#include "tether/estimator.h"

#include <math.h>
#include <string.h>

void tether_est_reset(tether_estimator *e)
{
    memset(e, 0, sizeof(*e));
}

void tether_est_begin_breath(tether_estimator *e)
{
    e->sqq = e->sqv = e->svv = e->spq = e->spv = 0.0;
    e->n = 0;
}

void tether_est_add(tether_estimator *e, double p, double q, double v)
{
    e->sqq += q * q;
    e->sqv += q * v;
    e->svv += v * v;
    e->spq += p * q;
    e->spv += p * v;
    e->n++;
}

int tether_est_end_breath(tether_estimator *e, double c_min, double c_max, double r_min, double r_max)
{
    if (e->n < 20) return 0;
    /* Normal equations for p = R*q + E*v (E = 1/C). */
    double det = e->sqq * e->svv - e->sqv * e->sqv;
    double scale = e->sqq * e->svv;
    if (!(scale > 0) || fabs(det) < 1e-9 * scale) return 0;
    double r = (e->spq * e->svv - e->spv * e->sqv) / det;
    double el = (e->sqq * e->spv - e->sqv * e->spq) / det;
    if (!(el > 0) || !isfinite(r) || !isfinite(el)) return 0;
    double c = 1.0 / el;
    if (c < c_min || c > c_max || r < r_min || r > r_max) return 0;
    e->r = r;
    e->c = c;
    e->valid++;
    return 1;
}
