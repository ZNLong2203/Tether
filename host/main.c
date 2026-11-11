/*
 * tether-sim — run Tether's arbiter against the simulated lung in simulated
 * time. Single scenarios (with CSV output for plotting) and the full
 * fault-injection campaign that produces the evidence table.
 *
 *   tether-sim --model models/vent-v1.tmlp --fault nan --at 10 --duration 30
 *   tether-sim --model models/vent-v1.tmlp --campaign
 * SPDX-License-Identifier: Apache-2.0
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sim.h"

static void usage(void)
{
    fputs("usage: tether-sim --model FILE [options]\n"
          "  --ctrl ai|raw|recovery|teacher   controller (default ai)\n"
          "  --fault NAME --at SEC [--until SEC]   inject a fault (see below)\n"
          "  --lung healthy|ards|asthma       patient (default healthy)\n"
          "  --lung-change ards|asthma|healthy --change-at SEC   change the patient mid-run\n"
          "  --duration SEC                   default 60\n"
          "  --csv FILE                       write per-tick samples\n"
          "  --campaign                       run every fault on every controller\n"
          "faults: crash hang slow nan spike drift stuck (hog and bypass are\n"
          "        process/OS-level and only meaningful on QNX)\n", stderr);
}

static void set_lung(tether_lung *l, const char *name)
{
    if (tether_lung_preset(l, name) != 0) {
        fprintf(stderr, "unknown lung '%s'\n", name);
        exit(2);
    }
}

static const char *ctrl_name(sim_controller c)
{
    switch (c) {
    case SIM_CTRL_AI: return "AI+Tether";
    case SIM_CTRL_AI_RAW: return "AI alone";
    case SIM_CTRL_RECOVERY: return "recovery";
    default: return "teacher";
    }
}

static void print_result(const sim_scenario *s, const sim_result *r)
{
    printf("controller        %s\n", ctrl_name(s->ctrl));
    printf("fault             %s at %.1fs\n", tether_fault_name(s->fault), s->fault_at_s);
    printf("breaths           %d\n", r->breaths);
    printf("peak pressure     %.1f cmH2O (limit %.0f)\n", r->max_paw, s->cfg.env.p_max);
    printf("peak volume       %.3f L above PEEP baseline (limit %.2f)\n", r->max_vol, s->cfg.env.vt_max);
    printf("time over limits  %.3f s pressure, %.3f s volume\n", r->time_over_pmax_s, r->time_over_vtmax_s);
    printf("switches to SAFE  %llu (readmissions %llu), final mode %s\n",
           (unsigned long long)r->switches_to_safe, (unsigned long long)r->readmissions,
           tether_mode_name((tether_mode)r->final_mode));
    if (r->first_switch_latency_s >= 0)
        printf("fault -> SAFE     %.1f ms\n", r->first_switch_latency_s * 1000.0);
    printf("tidal volume      %.3f L mean over last 40%% (target %.3f, error %.1f%%)\n",
           r->vt_mean_after, s->cfg.vent.vt_target, r->vt_err_mean_after * 100.0);
    for (int i = 1; i < TETHER_R_COUNT; i++)
        if (r->reason_count[i])
            printf("  rejected for %-26s x%llu\n", tether_reason_name((tether_reason)i),
                   (unsigned long long)r->reason_count[i]);
}

static void write_csv(const char *path, const sim_result *r)
{
    FILE *f = fopen(path, "w");
    if (!f) {
        perror(path);
        return;
    }
    fputs("t,paw,flow,vol,u,u_ai,mode,verdict,phase,violation,breath\n", f);
    for (size_t i = 0; i < r->n_samples; i += 2) {
        const tether_sample *x = &r->samples[i];
        fprintf(f, "%.3f,%.3f,%.4f,%.4f,%.3f,", x->tick / 1000.0, x->paw, x->flow, x->vol, x->u);
        if (isfinite(x->u_ai)) fprintf(f, "%.3f", x->u_ai);
        fprintf(f, ",%d,%d,%d,%d,%u\n", x->mode, x->verdict, x->phase, x->violation, x->breath);
    }
    fclose(f);
}

#define ONSETS 8
static void campaign(sim_scenario base, int markdown)
{
    static const tether_fault faults[] = { TETHER_FAULT_NONE, TETHER_FAULT_CRASH, TETHER_FAULT_HANG,
                                           TETHER_FAULT_SLOW, TETHER_FAULT_NAN, TETHER_FAULT_SPIKE,
                                           TETHER_FAULT_DRIFT, TETHER_FAULT_STUCK };
    static const sim_controller ctrls[] = { SIM_CTRL_AI_RAW, SIM_CTRL_AI };
    base.duration_s = 40.0;
    if (markdown) {
        puts("| fault | controller | peak paw (cmH2O) | peak vol (L) | s over limit | min breath vt (L) | fault to SAFE | vt error after fault clears |");
        puts("|---|---|---:|---:|---:|---:|---:|---:|");
    } else {
        printf("%-7s %-10s %9s %9s %11s %11s %12s %9s\n", "fault", "controller", "peak paw", "peak vol",
               "s over lim", "min vt (L)", "fault->SAFE", "vt error");
    }
    for (size_t fi = 0; fi < sizeof(faults) / sizeof(faults[0]); fi++) {
        for (size_t ci = 0; ci < sizeof(ctrls) / sizeof(ctrls[0]); ci++) {
            /* Worst case over ONSETS fault start times spread across one breath. */
            double paw = 0, vol = 0, over = 0, vtmin = 1e9, lat = -1, err = 0;
            int missed = 0;
            for (int o = 0; o < ONSETS; o++) {
                sim_scenario s = base;
                s.fault = faults[fi];
                s.ctrl = ctrls[ci];
                s.fault_at_s = 12.0 + o * (60.0 / s.cfg.vent.rr) / ONSETS;
                s.fault_until_s = s.fault_at_s + 8.0; /* the fault clears; is the AI let back in? */
                sim_result r;
                sim_run(&s, &r);
                if (r.max_paw > paw) paw = r.max_paw;
                if (r.max_vol > vol) vol = r.max_vol;
                if (r.time_over_pmax_s + r.time_over_vtmax_s > over) over = r.time_over_pmax_s + r.time_over_vtmax_s;
                if (s.fault != TETHER_FAULT_NONE && r.vt_min_after_fault < vtmin) vtmin = r.vt_min_after_fault;
                if (r.vt_err_mean_after > err) err = r.vt_err_mean_after;
                if (s.fault != TETHER_FAULT_NONE && s.ctrl == SIM_CTRL_AI) {
                    if (r.first_switch_latency_s < 0) missed++;
                    else if (r.first_switch_latency_s > lat) lat = r.first_switch_latency_s;
                }
                sim_free(&r);
            }
            char latb[32] = "-", vtb[16] = "-";
            if (lat >= 0 || missed) {
                if (missed) snprintf(latb, sizeof(latb), "MISSED x%d", missed);
                else snprintf(latb, sizeof(latb), "%.1f ms", lat * 1000.0);
            }
            if (vtmin < 1e8) snprintf(vtb, sizeof(vtb), "%.3f", vtmin);
            if (markdown)
                printf("| %s | %s | %.1f | %.3f | %.3f | %s | %s | %.1f%% |\n", tether_fault_name(faults[fi]),
                       ctrl_name(ctrls[ci]), paw, vol, over, vtb, latb, err * 100.0);
            else
                printf("%-7s %-10s %9.1f %9.3f %11.3f %11s %12s %8.1f%%\n", tether_fault_name(faults[fi]),
                       ctrl_name(ctrls[ci]), paw, vol, over, vtb, latb, err * 100.0);
        }
    }
    printf("%sworst case over %d fault-onset times within one breath; lung: %s\n", markdown ? "\n" : "",
           ONSETS, "as selected");
}

int main(int argc, char **argv)
{
    tether_config cfg;
    tether_config_defaults(&cfg);
    char err[200];
    if (tether_config_validate(&cfg, err, sizeof(err)) != 0) {
        fprintf(stderr, "config: %s\n", err);
        return 2;
    }

    const char *model_path = NULL, *csv = NULL;
    const char *lung = "healthy", *lung2 = NULL;
    sim_controller ctrl = SIM_CTRL_AI;
    tether_fault fault = TETHER_FAULT_NONE;
    double at = 10.0, until = 0.0, duration = 60.0, change_at = 0.0;
    int do_campaign = 0, markdown = 0;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        const char *v = i + 1 < argc ? argv[i + 1] : NULL;
        if (!strcmp(a, "--model") && v) model_path = argv[++i];
        else if (!strcmp(a, "--csv") && v) csv = argv[++i];
        else if (!strcmp(a, "--lung") && v) lung = argv[++i];
        else if (!strcmp(a, "--lung-change") && v) lung2 = argv[++i];
        else if (!strcmp(a, "--change-at") && v) change_at = atof(argv[++i]);
        else if (!strcmp(a, "--fault") && v) {
            fault = tether_fault_from_name(argv[++i]);
            if (fault == TETHER_FAULT_COUNT) {
                usage();
                return 2;
            }
        } else if (!strcmp(a, "--at") && v) at = atof(argv[++i]);
        else if (!strcmp(a, "--until") && v) until = atof(argv[++i]);
        else if (!strcmp(a, "--duration") && v) duration = atof(argv[++i]);
        else if (!strcmp(a, "--campaign")) do_campaign = 1;
        else if (!strcmp(a, "--markdown")) markdown = 1;
        else if (!strcmp(a, "--ctrl") && v) {
            v = argv[++i];
            if (!strcmp(v, "ai")) ctrl = SIM_CTRL_AI;
            else if (!strcmp(v, "raw")) ctrl = SIM_CTRL_AI_RAW;
            else if (!strcmp(v, "recovery")) ctrl = SIM_CTRL_RECOVERY;
            else if (!strcmp(v, "teacher")) ctrl = SIM_CTRL_TEACHER;
            else {
                usage();
                return 2;
            }
        } else {
            usage();
            return 2;
        }
    }

    static tether_mlp model;
    if (model_path == NULL && ctrl != SIM_CTRL_RECOVERY) {
        usage();
        return 2;
    }
    if (model_path != NULL && tether_mlp_load_file(&model, model_path, err, sizeof(err)) != 0) {
        fprintf(stderr, "model: %s\n", err);
        return 2;
    }

    sim_scenario s;
    sim_defaults(&s, &cfg, &model);
    set_lung(&s.lung, lung);
    s.lung2 = s.lung;
    if (lung2) {
        set_lung(&s.lung2, lung2);
        s.lung_change_s = change_at;
    }
    s.ctrl = ctrl;
    s.fault = fault;
    s.fault_at_s = at;
    s.fault_until_s = until;
    s.duration_s = duration;
    s.record = csv != NULL;

    if (do_campaign) {
        campaign(s, markdown);
        return 0;
    }
    sim_result r;
    sim_run(&s, &r);
    print_result(&s, &r);
    if (csv) write_csv(csv, &r);
    sim_free(&r);
    return 0;
}
