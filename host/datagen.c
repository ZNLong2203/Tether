/*
 * tether-datagen — generate imitation-learning data for the AI controller.
 *
 * Runs the real plant and the real arbiter bookkeeping with a privileged
 * teacher that knows the patient's true resistance and compliance. Each
 * breath the applied command is randomly scaled (exploration) while the
 * label stays the teacher's, so the data covers the states a learner passes
 * through after the lung changes. Output: float32 rows of
 * [TETHER_POLICY_NIN features, label = driving pressure above PEEP].
 * SPDX-License-Identifier: Apache-2.0
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tether/arbiter.h"
#include "tether/plant.h"
#include "tether/policy.h"

static uint64_t rng_state = 88172645463325252ULL;
static double urand(void)
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 7;
    rng_state ^= rng_state << 17;
    return (double)(rng_state >> 11) * (1.0 / 9007199254740992.0);
}
static double urange(double a, double b) { return a + (b - a) * urand(); }
/* log-uniform */
static double lrange(double a, double b) { return exp(urange(log(a), log(b))); }

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : "dataset.f32";
    int episodes = argc > 2 ? atoi(argv[2]) : 300;
    if (argc > 3) rng_state ^= (uint64_t)atoll(argv[3]) * 0x9E3779B97F4A7C15ULL;

    FILE *f = fopen(path, "wb");
    if (f == NULL) {
        perror(path);
        return 1;
    }

    tether_config cfg;
    tether_config_defaults(&cfg);
    /* Permissive envelope: this is a teacher run, not a safety run. */
    cfg.env.p_max = 100.0;
    cfg.env.vt_max = 100.0;
    cfg.env.rise_step_max = 100.0;
    cfg.env.exp_tolerance = 100.0;
    cfg.env.max_age_s = 10.0;
    cfg.env.readmit_ticks = 1;
    cfg.env.p_min = 0.0;

    long rows = 0;
    for (int ep = 0; ep < episodes; ep++) {
        tether_lung lung;
        tether_lung_defaults(&lung);
        lung.resistance = lrange(4.0, 30.0);
        lung.compliance = lrange(0.020, 0.058);
        lung.noise = urange(0.0, 0.15);
        lung.effort_amp = urand() < 0.3 ? urange(0.0, 3.0) : 0.0;

        tether_arbiter a;
        tether_arbiter_init(&a, &cfg);
        tether_plant plant;
        tether_plant_init(&plant, &lung, cfg.vent.peep, 1 + (uint64_t)ep);

        double change_at = urand() < 0.7 ? urange(8.0, 30.0) : 1e9;
        double duration = 45.0;
        double factor = 1.0;
        int recovery_breath = 0;
        uint32_t last_breath = 0xffffffffu;
        tether_proposal prop;
        memset(&prop, 0, sizeof(prop));

        uint64_t n = (uint64_t)(duration * cfg.timing.control_hz);
        for (uint64_t k = 0; k < n; k++) {
            double t = (double)k / cfg.timing.control_hz;
            int64_t now = (int64_t)(t * 1e9);
            if (t >= change_at) {
                change_at = 1e9;
                lung.compliance = lrange(0.020, 0.058);
                lung.resistance = lrange(4.0, 30.0);
            }
            tether_sensor_reading rd = tether_plant_read(&plant, &lung);
            tether_observation obs;
            tether_arbiter_observe(&a, now, &obs);
            if (obs.breath != last_breath) {
                last_breath = obs.breath;
                factor = urand() < 0.4 ? 1.0 : urange(0.5, 1.6);
                /* Some breaths are driven by the recovery controller, as they are
                 * after Tether switches: the AI must learn to compensate for those. */
                recovery_breath = urand() < 0.2;
            }
            double dp_t = tether_teacher_dp(&obs, lung.resistance, lung.compliance);
            prop.u = cfg.vent.peep + factor * dp_t;
            if (recovery_breath) {
                double ramp = obs.t_in_phase >= cfg.vent.rise_time ? 1.0 : obs.t_in_phase / cfg.vent.rise_time;
                prop.u = obs.phase == 0 ? cfg.vent.peep + cfg.rec.p_insp_safe * ramp : cfg.vent.peep;
            }
            prop.seq++;
            prop.recv_ns = now;
            prop.valid = 1;

            if (obs.breath >= 3 && k % 20 == 0) {
                float row[TETHER_POLICY_NIN + 1];
                tether_policy_features(&obs, row);
                row[TETHER_POLICY_NIN] = (float)dp_t;
                fwrite(row, sizeof(float), TETHER_POLICY_NIN + 1, f);
                rows++;
            }

            tether_decision d = tether_arbiter_step(&a, rd.paw, rd.flow, &prop, now, 1);
            tether_plant_step(&plant, &lung, d.u, 1.0 / cfg.timing.control_hz);
        }
    }
    fclose(f);
    fprintf(stderr, "wrote %ld rows (%d features + label) to %s\n", rows, TETHER_POLICY_NIN, path);
    return 0;
}
