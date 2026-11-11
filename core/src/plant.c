/* SPDX-License-Identifier: Apache-2.0 */
#include "tether/plant.h"

#include <math.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

void tether_lung_defaults(tether_lung *lung)
{
    lung->resistance = 10.0;
    lung->compliance = 0.050;
    lung->blower_tau = 0.015;
    lung->effort_amp = 0.0;
    lung->effort_rr = 18.0;
    lung->noise = 0.0;
}

int tether_lung_preset(tether_lung *lung, const char *name)
{
    tether_lung_defaults(lung);
    lung->noise = 0.05;
    if (strcmp(name, "healthy") == 0) return 0;
    if (strcmp(name, "ards") == 0) {
        lung->compliance = 0.025;
        lung->resistance = 12.0;
        return 0;
    }
    if (strcmp(name, "asthma") == 0) {
        lung->compliance = 0.045;
        lung->resistance = 25.0;
        return 0;
    }
    return -1;
}

void tether_plant_init(tether_plant *p, const tether_lung *lung, double peep, uint64_t seed)
{
    p->t = 0.0;
    p->pb = peep;
    p->v = lung->compliance * peep;
    p->q = 0.0;
    p->pmus = 0.0;
    p->rng = seed ? seed : 0x9E3779B97F4A7C15ULL;
}

static double patient_effort(const tether_lung *lung, double t)
{
    if (lung->effort_amp <= 0.0 || lung->effort_rr <= 0.0) return 0.0;
    double period = 60.0 / lung->effort_rr;
    double ph = fmod(t, period) / period;
    /* Effort occupies the first 35% of the patient's own cycle. */
    if (ph >= 0.35) return 0.0;
    double s = sin(M_PI * ph / 0.35);
    return lung->effort_amp * s * s;
}

void tether_plant_step(tether_plant *p, const tether_lung *lung, double u, double dt)
{
    if (!(dt > 0)) return;
    if (!isfinite(u)) u = p->pb; /* hardware ignores garbage; the monitor never sends it anyway */
    int n = (int)ceil(dt / 1e-4);
    double h = dt / n;
    for (int i = 0; i < n; i++) {
        p->pmus = patient_effort(lung, p->t);
        /* Semi-implicit: lag first, then the lung with the new pressure. */
        p->pb += (u - p->pb) * (h / (lung->blower_tau + h));
        double q = (p->pb - p->v / lung->compliance + p->pmus) / lung->resistance;
        /* Implicit update of V for stiffness: V' = (pb + pmus - V/C)/R */
        double k = h / (lung->resistance * lung->compliance);
        double v_ss = lung->compliance * (p->pb + p->pmus);
        p->v = (p->v + k * v_ss) / (1.0 + k);
        p->q = q;
        p->t += h;
    }
    p->q = (p->pb - p->v / lung->compliance + p->pmus) / lung->resistance;
}

static double gauss(uint64_t *s)
{
    /* xorshift64* + Box-Muller; deterministic for a given seed */
    double u1, u2;
    do {
        *s ^= *s >> 12; *s ^= *s << 25; *s ^= *s >> 27;
        u1 = (double)((*s * 0x2545F4914F6CDD1DULL) >> 11) * (1.0 / 9007199254740992.0);
    } while (u1 <= 1e-12);
    *s ^= *s >> 12; *s ^= *s << 25; *s ^= *s >> 27;
    u2 = (double)((*s * 0x2545F4914F6CDD1DULL) >> 11) * (1.0 / 9007199254740992.0);
    return sqrt(-2.0 * log(u1)) * cos(2.0 * M_PI * u2);
}

tether_sensor_reading tether_plant_read(tether_plant *p, const tether_lung *lung)
{
    tether_sensor_reading r = { p->pb, p->q };
    if (lung->noise > 0.0) {
        r.paw += lung->noise * gauss(&p->rng);
        r.flow += lung->noise * 0.01 * gauss(&p->rng);
    }
    return r;
}
