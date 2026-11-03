/* Unit and property tests for the portable core. SPDX-License-Identifier: Apache-2.0 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sim.h"
#include "tether/arbiter.h"
#include "tether/config.h"
#include "tether/estimator.h"
#include "tether/plant.h"
#include "tether/policy.h"
#include "tether/rt.h"

static int failures, checks;
#define CHECK(cond, ...)                                                    \
    do {                                                                    \
        checks++;                                                           \
        if (!(cond)) {                                                      \
            failures++;                                                     \
            fprintf(stderr, "FAIL %s:%d: %s -- ", __FILE__, __LINE__, #cond); \
            fprintf(stderr, __VA_ARGS__);                                   \
            fputc('\n', stderr);                                            \
        }                                                                   \
    } while (0)

static const char *model_path;
static tether_mlp model;

static uint64_t rs = 0x1234567;
static double urand(void)
{
    rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17;
    return (double)(rs >> 11) * (1.0 / 9007199254740992.0);
}

static void test_config(void)
{
    tether_config c;
    char err[200];
    tether_config_defaults(&c);
    CHECK(tether_config_validate(&c, err, sizeof err) == 0, "defaults invalid: %s", err);

    /* A recovery pressure that could over-inflate the most compliant lung. */
    tether_config bad = c;
    bad.rec.p_insp_safe = 14.0;
    CHECK(tether_config_validate(&bad, err, sizeof err) != 0, "unsafe recovery pressure accepted");
    CHECK(strstr(err, "vt_max") != NULL, "error should name vt_max, got: %s", err);

    bad = c;
    bad.rec.p_insp_safe = 31.0; /* PEEP + 31 > p_max */
    CHECK(tether_config_validate(&bad, err, sizeof err) != 0, "recovery above p_max accepted");

    bad = c;
    bad.env.max_age_s = 0.001; /* shorter than one AI period */
    CHECK(tether_config_validate(&bad, err, sizeof err) != 0, "impossible freshness deadline accepted");

    bad = c;
    bad.vent.vt_target = bad.env.vt_max * 1.1;
    CHECK(tether_config_validate(&bad, err, sizeof err) != 0, "target above ceiling accepted");

    bad = c;
    bad.env.c_margin = 0.9; /* optimistic margin */
    CHECK(tether_config_validate(&bad, err, sizeof err) != 0, "optimistic margin accepted");
}

static void test_plant(void)
{
    tether_lung lung;
    tether_lung_defaults(&lung);
    lung.resistance = 10.0;
    lung.compliance = 0.040;
    lung.blower_tau = 1e-3; /* make the lag negligible */
    tether_plant p;
    tether_plant_init(&p, &lung, 5.0, 1);
    CHECK(fabs(p.v - 0.040 * 5.0) < 1e-12, "initial volume");

    /* Step to 15 cmH2O. Volume must approach C*15 with time constant R*C = 0.4 s. */
    double t = 0, v0 = p.v;
    for (int i = 0; i < 400; i++) { tether_plant_step(&p, &lung, 15.0, 1e-3); t += 1e-3; }
    double expect = 0.040 * 15.0 + (v0 - 0.040 * 15.0) * exp(-t / 0.4);
    CHECK(fabs(p.v - expect) < 0.004, "RC response: got %.4f want %.4f", p.v, expect);
    for (int i = 0; i < 4000; i++) tether_plant_step(&p, &lung, 15.0, 1e-3);
    CHECK(fabs(p.v - 0.040 * 15.0) < 1e-4, "steady state volume %.5f", p.v);
    CHECK(fabs(p.q) < 1e-3, "steady state flow %.5f", p.q);

    /* Garbage on the actuator must not poison the physics. */
    tether_plant_step(&p, &lung, NAN, 1e-3);
    CHECK(isfinite(p.v) && isfinite(p.pb) && isfinite(p.q), "NaN command leaked into the plant");
}

static void test_estimator(void)
{
    tether_config cfg;
    tether_config_defaults(&cfg);
    const double rs_true[] = { 5, 10, 20, 30 };
    const double cs_true[] = { 0.025, 0.040, 0.055 };
    for (int i = 0; i < 4; i++) for (int j = 0; j < 3; j++) {
        tether_lung lung;
        tether_lung_defaults(&lung);
        lung.resistance = rs_true[i];
        lung.compliance = cs_true[j];
        lung.noise = 0.05;
        tether_arbiter a;
        tether_arbiter_init(&a, &cfg);
        tether_plant pl;
        tether_plant_init(&pl, &lung, cfg.vent.peep, 3);
        /* Open-loop recovery controller for three breaths: no AI needed. */
        for (int k = 0; k < 3 * 4000; k++) {
            tether_sensor_reading rd = tether_plant_read(&pl, &lung);
            tether_decision d = tether_arbiter_step(&a, rd.paw, rd.flow, NULL, (int64_t)k * 1000000, 1);
            tether_plant_step(&pl, &lung, d.u, 1e-3);
        }
        CHECK(a.est.valid > 0, "no estimate for R=%.0f C=%.3f", rs_true[i], cs_true[j]);
        CHECK(fabs(a.est.c - cs_true[j]) / cs_true[j] < 0.15, "C est %.4f true %.4f (R=%.0f)", a.est.c, cs_true[j], rs_true[i]);
        CHECK(fabs(a.est.r - rs_true[i]) / rs_true[i] < 0.30, "R est %.2f true %.2f (C=%.3f)", a.est.r, rs_true[i], cs_true[j]);
    }
}

static void test_monitor_verdicts(void)
{
    tether_config cfg;
    tether_config_defaults(&cfg);
    tether_arbiter a;
    tether_arbiter_init(&a, &cfg);
    a.est.valid = 1; a.est.c = 0.04; a.est.r = 10; /* pretend characterised */
    a.phase = 0;
    a.u_applied = 12.0;
    int64_t now = 1000000000;
    tether_proposal p = { .u = 12.0, .seq = 1, .recv_ns = now, .valid = 1 };

    CHECK(tether_monitor_check(&a, &p, now, 1) == TETHER_OK, "healthy proposal rejected");
    CHECK(tether_monitor_check(&a, NULL, now, 1) == TETHER_R_NO_PROPOSAL, "no proposal");
    CHECK(tether_monitor_check(&a, &p, now, 0) == TETHER_R_DISCONNECTED, "disconnect");
    CHECK(tether_monitor_check(&a, &p, now + 30000000, 1) == TETHER_R_STALE, "stale");
    p.u = NAN;
    CHECK(tether_monitor_check(&a, &p, now, 1) == TETHER_R_NONFINITE, "nan");
    p.u = INFINITY;
    CHECK(tether_monitor_check(&a, &p, now, 1) == TETHER_R_NONFINITE, "inf");
    p.u = 70.0;
    CHECK(tether_monitor_check(&a, &p, now, 1) == TETHER_R_RANGE, "range hi");
    p.u = -4.0;
    CHECK(tether_monitor_check(&a, &p, now, 1) == TETHER_R_RANGE, "range lo");
    p.u = 20.0; /* +8 over applied 12 */
    CHECK(tether_monitor_check(&a, &p, now, 1) == TETHER_R_RISE, "rise");

    /* Expiration: pressure well above PEEP is rejected once the grace window is over. */
    a.phase = 1;
    a.phase_ns = now - 100000000;
    p.u = 15.0;
    CHECK(tether_monitor_check(&a, &p, now, 1) == TETHER_R_EXP_PHASE, "expiration pressure");
    a.phase_ns = now - 5000000; /* just flipped: proposal may describe the previous phase */
    CHECK(tether_monitor_check(&a, &p, now, 1) == TETHER_OK, "grace window");
    p.u = 5.0;
    a.phase_ns = now - 100000000;
    CHECK(tether_monitor_check(&a, &p, now, 1) == TETHER_OK, "PEEP in expiration");

    /* Model lookahead: a high but in-range, in-step pressure that the
     * estimated lung could not take. */
    a.phase = 0; a.phase_tick = a.tick; a.vol = 0.0; a.u_applied = 28.0;
    p.u = 30.0;
    CHECK(tether_monitor_check(&a, &p, now, 1) == TETHER_R_PREDICT_VOL, "lookahead");
}

static void test_arbiter_modes(void)
{
    tether_config cfg;
    tether_config_defaults(&cfg);
    tether_lung lung;
    tether_lung_defaults(&lung);
    tether_arbiter a;
    tether_arbiter_init(&a, &cfg);
    tether_plant pl;
    tether_plant_init(&pl, &lung, cfg.vent.peep, 1);

    CHECK(a.mode == TETHER_MODE_SAFE && a.reason == TETHER_R_STARTUP, "starts in SAFE/startup");

    /* A perfect teacher is admitted after the startup breath and then stays. */
    tether_proposal p;
    memset(&p, 0, sizeof p);
    int64_t now;
    int switches_after_admission = 0;
    int admitted = 0;
    for (int k = 0; k < 8 * 4000; k++) {
        now = (int64_t)k * 1000000;
        tether_sensor_reading rd = tether_plant_read(&pl, &lung);
        tether_observation o;
        tether_arbiter_observe(&a, now, &o);
        p.u = tether_teacher_command(&o, lung.resistance, lung.compliance);
        p.recv_ns = now; p.seq++; p.valid = 1;
        tether_decision d = tether_arbiter_step(&a, rd.paw, rd.flow, &p, now, 1);
        if (d.mode == TETHER_MODE_AI) admitted = 1;
        if (admitted && d.switched && d.mode == TETHER_MODE_SAFE) switches_after_admission++;
        tether_plant_step(&pl, &lung, d.u, 1e-3);
    }
    CHECK(admitted, "teacher never admitted");
    CHECK(switches_after_admission == 0, "good AI was switched out %d times", switches_after_admission);
    CHECK(a.violations == 0, "violations with a good AI: %llu", (unsigned long long)a.violations);

    /* Readmission is sticky: only at a breath boundary, after N healthy ticks. */
    uint64_t readm = a.readmissions;
    now += 1000000;
    p.u = NAN; p.recv_ns = now;
    tether_decision d = tether_arbiter_step(&a, 5, 0, &p, now, 1);
    CHECK(d.switched && d.mode == TETHER_MODE_SAFE && d.verdict == TETHER_R_NONFINITE, "NaN must switch to SAFE");
    CHECK(isfinite(d.u) && d.u >= cfg.vent.peep - 1e-9 && d.u <= cfg.vent.peep + cfg.rec.p_insp_safe + 1e-9,
          "recovery command out of bounds: %f", d.u);
    CHECK(a.readmissions == readm, "readmitted instantly");
}

/* The central claim: whatever the AI says, the patient stays inside the envelope.
 * Adversarial AI: random values, NaN, spikes, extremes, rapid changes; patients
 * spanning the envelope; lung changes; sensor noise and patient effort. */
static void test_hostile_ai_cannot_breach_envelope(void)
{
    tether_config cfg;
    tether_config_defaults(&cfg);
    int worst_mode_ticks = 0;
    double worst_p = 0, worst_v = 0;
    uint64_t total_violation_ticks = 0;
    long cases = 0;

    for (int trial = 0; trial < 60; trial++) {
        tether_lung lung;
        tether_lung_defaults(&lung);
        lung.resistance = 5.0 + urand() * 30.0;
        lung.compliance = 0.020 + urand() * 0.038;
        lung.noise = urand() * 0.15;
        lung.effort_amp = urand() < 0.3 ? urand() * 3.0 : 0.0;
        tether_arbiter a;
        tether_arbiter_init(&a, &cfg);
        tether_plant pl;
        tether_plant_init(&pl, &lung, cfg.vent.peep, 100 + trial);
        tether_proposal p;
        memset(&p, 0, sizeof p);

        double style = urand();
        double hold_u = 5.0;
        for (int k = 0; k < 40 * 1000; k++) {
            int64_t now = (int64_t)k * 1000000;
            if (k == 20 * 1000 && urand() < 0.5) {
                lung.resistance = 5.0 + urand() * 30.0;
                lung.compliance = 0.020 + urand() * 0.038;
            }
            tether_sensor_reading rd = tether_plant_read(&pl, &lung);
            if (k % 10 == 0 && urand() < 0.97) {
                tether_observation o;
                tether_arbiter_observe(&a, now, &o);
                double good = tether_teacher_command(&o, lung.resistance, lung.compliance);
                double r = urand();
                if (style < 0.25) hold_u = -20.0 + urand() * 100.0;                   /* white noise */
                else if (style < 0.5) hold_u = r < 0.85 ? good : (r < 0.9 ? NAN : 25.0 + urand() * 40.0);
                else if (style < 0.75) hold_u = good * (1.0 + 0.04 * (k / 1000.0) * r);   /* growing gain error */
                else hold_u = r < 0.5 ? good : (r < 0.75 ? 34.9 : 3.1);                  /* sit on the limits */
                p.u = hold_u;
                p.recv_ns = now;
                p.seq++;
                p.valid = 1;
            }
            tether_decision d = tether_arbiter_step(&a, rd.paw, rd.flow, &p, now, 1);
            tether_plant_step(&pl, &lung, d.u, 1e-3);
            double vol = pl.v - lung.compliance * cfg.vent.peep;
            if (pl.pb > worst_p) worst_p = pl.pb;
            if (vol > worst_v) worst_v = vol;
            if (k > 2000) { /* after the first breath the monitor has characterised the patient */
                if (pl.pb > cfg.env.p_max) worst_mode_ticks++;
                if (vol > cfg.env.vt_max) worst_mode_ticks++;
            }
            cases++;
        }
        total_violation_ticks += a.violations;
    }
    CHECK(worst_mode_ticks == 0, "envelope breached on %d ticks (peak %.1f cmH2O, %.3f L)", worst_mode_ticks, worst_p, worst_v);
    CHECK(total_violation_ticks == 0, "arbiter measured %llu violating ticks", (unsigned long long)total_violation_ticks);
    printf("  hostile-AI fuzz: %ld ticks, peak %.1f cmH2O (limit %.0f), peak %.3f L (limit %.2f)\n",
           cases, worst_p, cfg.env.p_max, worst_v, cfg.env.vt_max);
}

static void test_policy_and_model(void)
{
    char err[200];
    CHECK(tether_mlp_load_file(&model, model_path, err, sizeof err) == 0, "load model: %s", err);
    CHECK(model.model_id == 1, "model id %u", model.model_id);

    /* Truncation anywhere must be rejected, never read out of bounds. */
    FILE *f = fopen(model_path, "rb");
    static unsigned char buf[8192];
    size_t n = f ? fread(buf, 1, sizeof buf, f) : 0;
    if (f) fclose(f);
    CHECK(n > 100, "model file too small");
    int accepted_truncated = 0;
    for (size_t cut = 0; cut + 1 < n; cut += 7) {
        tether_mlp m;
        if (tether_mlp_load_mem(&m, buf, cut, NULL, 0) == 0) accepted_truncated++;
    }
    CHECK(accepted_truncated == 0, "%d truncated files accepted", accepted_truncated);
    unsigned char bad[16] = "NOTAMODEL";
    tether_mlp m;
    CHECK(tether_mlp_load_mem(&m, bad, sizeof bad, err, sizeof err) != 0, "garbage accepted");
    CHECK(tether_mlp_load_file(&m, "/nonexistent/file.tmlp", err, sizeof err) != 0, "missing file accepted");

    /* The learned policy tracks the teacher closely on states it will see. */
    tether_config cfg;
    tether_config_defaults(&cfg);
    tether_observation o;
    memset(&o, 0, sizeof o);
    o.peep = cfg.vent.peep; o.ti = cfg.vent.ti; o.vt_target = cfg.vent.vt_target; o.rise_time = cfg.vent.rise_time;
    o.t_in_phase = 0.6; o.paw = 5 + 17; o.flow = 0.1; o.vol = 0.3;
    o.vt_last = cfg.vent.vt_target; o.dp_last = 17.0;
    double u = tether_policy_command(&model, &o);
    CHECK(isfinite(u) && u > 15 && u < 30, "policy output %f implausible", u);
    o.phase = 1; o.t_in_phase = 0.5; o.paw = 5.0; o.flow = -0.2;
    u = tether_policy_command(&model, &o);
    CHECK(fabs(u - cfg.vent.peep) < 1.0, "policy should return to PEEP in expiration, got %f", u);
}

static void test_fault_names(void)
{
    for (int i = 0; i < TETHER_FAULT_COUNT; i++) {
        const char *n = tether_fault_name((tether_fault)i);
        CHECK(tether_fault_from_name(n) == (tether_fault)i, "round trip %s", n);
    }
    CHECK(tether_fault_from_name("nonsense") == TETHER_FAULT_COUNT, "unknown fault name");
    CHECK(tether_fault_from_name("clear") == TETHER_FAULT_NONE, "clear alias");
}

static void test_rt_primitives(void)
{
    static tether_hist h;
    tether_hist_reset(&h);
    for (int i = 1; i <= 1000; i++) tether_hist_add(&h, (int64_t)i * 1000); /* 1..1000 us */
    int64_t p50 = tether_hist_percentile(&h, 50), p99 = tether_hist_percentile(&h, 99);
    CHECK(p50 >= 495000 && p50 <= 510000, "p50 %lld", (long long)p50);
    CHECK(p99 >= 985000 && p99 <= 1010000, "p99 %lld", (long long)p99);
    CHECK(h.max_ns == 1000000 && h.min_ns == 1000, "min/max");
    tether_hist_add(&h, 5000000000LL); /* 5 s outlier goes in the overflow bucket */
    CHECK(tether_hist_percentile(&h, 100) == 5000000000LL, "overflow keeps the maximum");

    static tether_ring ring;
    tether_ring_init(&ring);
    static tether_sample out[TETHER_RING_CAP];
    uint64_t cur = 0;
    for (int i = 0; i < 100; i++) { tether_sample s = { .tick = (uint64_t)i }; tether_ring_push(&ring, &s); }
    size_t n = tether_ring_read(&ring, &cur, out, 1000);
    CHECK(n == 100 && out[0].tick == 0 && out[99].tick == 99 && cur == 100, "ring read %zu", n);
    n = tether_ring_read(&ring, &cur, out, 1000);
    CHECK(n == 0, "ring re-read");
    for (int i = 100; i < 30000; i++) { tether_sample s = { .tick = (uint64_t)i }; tether_ring_push(&ring, &s); }
    n = tether_ring_read(&ring, &cur, out, TETHER_RING_CAP);
    CHECK(n > 0 && n < TETHER_RING_CAP && out[n - 1].tick == 29999, "lapped reader skips ahead (n=%zu)", n);
    for (size_t i = 1; i < n; i++) if (out[i].tick != out[i - 1].tick + 1) { CHECK(0, "gap in ring read"); break; }

    static tether_mailbox mb;
    tether_mailbox_init(&mb);
    tether_proposal in = { .u = 12.5, .seq = 7, .model_id = 3, .recv_ns = 99, .valid = 1 }, got;
    tether_mailbox_write(&mb, &in);
    CHECK(tether_mailbox_read(&mb, &got) == 1 && got.u == 12.5 && got.seq == 7, "mailbox round trip");
}

static void test_json(void)
{
    tether_config cfg;
    tether_config_defaults(&cfg);
    tether_arbiter a;
    tether_arbiter_init(&a, &cfg);
    static tether_hist w, e;
    tether_hist_reset(&w); tether_hist_reset(&e);
    tether_hist_add(&w, 12000);
    tether_eventlog log;
    memset(&log, 0, sizeof log);
    tether_switch_event ev = { .t_ns = 5, .tick = 5, .from_mode = 1, .to_mode = 0, .reason = TETHER_R_NONFINITE, .breath = 2 };
    tether_eventlog_push(&log, &ev);
    static tether_status st;
    tether_status_fill(&st, &a, &w, &e, &log, NULL, 100, 1);
    char buf[8192];
    int n = tether_status_json(&st, buf, sizeof buf);
    CHECK(n > 100, "status json length %d", n);
    int depth = 0, ok = 1;
    for (int i = 0; i < n; i++) {
        if (buf[i] == '{' || buf[i] == '[') depth++;
        if (buf[i] == '}' || buf[i] == ']') { depth--; if (depth < 0) ok = 0; }
    }
    CHECK(ok && depth == 0, "unbalanced status json");
    CHECK(strstr(buf, "\"non-finite\"") != NULL && strstr(buf, "nan") == NULL, "event reason / no NaN token");
    char tiny[32];
    CHECK(tether_status_json(&st, tiny, sizeof tiny) == -1, "overflow must be reported");

    tether_sample s[2] = { { .tick = 1, .paw = 5, .u = 5, .u_ai = NAN }, { .tick = 2, .paw = 6, .u = 6, .u_ai = 7 } };
    n = tether_samples_json(s, 2, 1, buf, sizeof buf);
    CHECK(n > 0 && strstr(buf, "null") != NULL && strstr(buf, "nan") == NULL, "samples json: %s", buf);
}

/* Regression for the whole campaign: with Tether, no fault on any lung may
 * push the patient past a limit. */
static void test_campaign_no_overshoot(void)
{
    static const char *lungs[] = { "healthy", "ards", "asthma" };
    tether_config cfg;
    tether_config_defaults(&cfg);
    for (int li = 0; li < 3; li++) {
        for (int f = 1; f < TETHER_FAULT_COUNT; f++) {
            if (f == TETHER_FAULT_HOG || f == TETHER_FAULT_BYPASS) continue;
            for (int o = 0; o < 8; o++) {
                sim_scenario s;
                sim_defaults(&s, &cfg, &model);
                tether_lung_defaults(&s.lung);
                s.lung.noise = 0.05;
                if (li == 1) { s.lung.compliance = 0.025; s.lung.resistance = 12; }
                if (li == 2) { s.lung.compliance = 0.045; s.lung.resistance = 25; }
                s.lung2 = s.lung;
                s.fault = (tether_fault)f;
                s.fault_at_s = 12.0 + o * 0.5;
                s.fault_until_s = s.fault_at_s + 8.0;
                s.duration_s = 40.0;
                sim_result r;
                sim_run(&s, &r);
                CHECK(r.time_over_pmax_s == 0.0 && r.time_over_vtmax_s == 0.0,
                      "%s on %s (onset %.1f): over limit %.3f s, peak %.1f / %.3f L", tether_fault_name((tether_fault)f),
                      lungs[li], s.fault_at_s, r.time_over_pmax_s + r.time_over_vtmax_s, r.max_paw, r.max_vol);
                CHECK(r.first_switch_latency_s >= 0, "%s on %s never switched to SAFE", tether_fault_name((tether_fault)f), lungs[li]);
                CHECK(r.first_switch_latency_s < 10.0, "%s took %.1f s", tether_fault_name((tether_fault)f), r.first_switch_latency_s);
                CHECK(r.final_mode == TETHER_MODE_AI, "%s on %s: AI not readmitted after the fault cleared", tether_fault_name((tether_fault)f), lungs[li]);
                sim_free(&r);
            }
        }
    }
}

int main(int argc, char **argv)
{
    model_path = argc > 1 ? argv[1] : "models/vent-v1.tmlp";
    static void (*const tests[])(void) = {
        test_config, test_plant, test_estimator, test_monitor_verdicts, test_arbiter_modes,
        test_fault_names, test_rt_primitives, test_json, test_policy_and_model,
        test_hostile_ai_cannot_breach_envelope, test_campaign_no_overshoot,
    };
    static const char *names[] = {
        "config", "plant", "estimator", "monitor verdicts", "arbiter modes", "fault names",
        "rt primitives", "json", "policy+model", "hostile AI fuzz", "campaign",
    };
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        int before = failures;
        tests[i]();
        printf("%-18s %s\n", names[i], failures == before ? "ok" : "FAILED");
    }
    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
