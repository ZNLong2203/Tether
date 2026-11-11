/* SPDX-License-Identifier: Apache-2.0 */
#include "qcommon.h"

#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/neutrino.h>
#include <sys/slog2.h>
#include <time.h>

#include "tether/common.h"

static slog2_buffer_t g_slog;
static pthread_once_t g_slog_once = PTHREAD_ONCE_INIT;

static void slog_init(void)
{
    slog2_buffer_set_config_t c;
    memset(&c, 0, sizeof(c));
    c.num_buffers = 1;
    c.buffer_set_name = "tether";
    c.verbosity_level = SLOG2_INFO;
    c.buffer_config[0].buffer_name = "main";
    c.buffer_config[0].num_pages = 4;
    if (slog2_register(&c, &g_slog, 0) != 0) g_slog = NULL;
}

void qlog(const char *tag, const char *fmt, ...)
{
    char msg[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    int64_t ns = tether_now_ns();
    fprintf(stderr, "[%6lld.%03lld] %-8s %s\n", (long long)(ns / TETHER_NS_PER_S),
            (long long)((ns % TETHER_NS_PER_S) / TETHER_NS_PER_MS), tag, msg);
    pthread_once(&g_slog_once, slog_init);
    if (g_slog != NULL) slog2f(g_slog, 0, SLOG2_INFO, "%s: %s", tag, msg);
}

void qlog_rt(const char *tag, const char *fmt, ...)
{
    char msg[200];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    pthread_once(&g_slog_once, slog_init);
    if (g_slog != NULL) slog2f(g_slog, 0, SLOG2_INFO, "%s: %s", tag, msg);
}

static int opt_val(const char *arg, const char *key, double *out)
{
    size_t n = strlen(key);
    if (strncmp(arg, key, n) != 0 || arg[n] != '=') return 0;
    char *end = NULL;
    double v = strtod(arg + n + 1, &end);
    if (end == arg + n + 1 || *end != '\0') return 0;
    *out = v;
    return 1;
}

int q_config_arg(const char *arg, tether_config *cfg)
{
    double v;
    if (opt_val(arg, "--control-hz", &v)) cfg->timing.control_hz = v;
    else if (opt_val(arg, "--ai-hz", &v)) cfg->timing.ai_hz = v;
    else if (opt_val(arg, "--plant-hz", &v)) cfg->timing.plant_hz = v;
    else if (opt_val(arg, "--max-age-ms", &v)) cfg->env.max_age_s = v / 1000.0;
    else if (opt_val(arg, "--readmit-ticks", &v)) cfg->env.readmit_ticks = (int)v;
    else if (opt_val(arg, "--hw-watchdog-ms", &v)) cfg->timing.hw_watchdog_s = v / 1000.0;
    else return 0;
    return 1;
}

int q_config_load(int argc, char **argv, tether_config *cfg, const char *tag)
{
    char err[160];
    tether_config_defaults(cfg);
    for (int i = 1; i < argc; i++) (void)q_config_arg(argv[i], cfg);
    if (tether_config_validate(cfg, err, sizeof(err)) != 0) {
        qlog(tag, "configuration refused: %s", err);
        return -1;
    }
    return 0;
}

int q_set_fifo(int prio)
{
    struct sched_param sp;
    memset(&sp, 0, sizeof(sp));
    sp.sched_priority = prio;
    return pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp);
}

int q_lock_memory(void)
{
    return mlockall(MCL_CURRENT | MCL_FUTURE) == 0 ? 0 : errno;
}

int q_clock_period_us(unsigned us)
{
    struct _clockperiod cp;
    cp.nsec = us * 1000u;
    cp.fract = 0;
    return ClockPeriod(CLOCK_REALTIME, &cp, NULL, 0) == 0 ? 0 : errno;
}
