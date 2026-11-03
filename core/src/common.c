/* SPDX-License-Identifier: Apache-2.0 */
#define _POSIX_C_SOURCE 200809L
#include "tether/common.h"

#include <string.h>
#include <time.h>

static const char *const reason_names[TETHER_R_COUNT] = {
    [TETHER_OK] = "ok",
    [TETHER_R_STARTUP] = "startup",
    [TETHER_R_NO_PROPOSAL] = "no-proposal",
    [TETHER_R_STALE] = "stale",
    [TETHER_R_DISCONNECTED] = "disconnected",
    [TETHER_R_NONFINITE] = "non-finite",
    [TETHER_R_RANGE] = "range",
    [TETHER_R_RISE] = "rise",
    [TETHER_R_EXP_PHASE] = "expiration-pressure",
    [TETHER_R_PREDICT_VOL] = "predicted-overdistension",
    [TETHER_R_VOLUME_LIMIT] = "volume-limit",
    [TETHER_R_MEASURED_PRESSURE] = "measured-pressure",
};

static const char *const fault_names[TETHER_FAULT_COUNT] = {
    [TETHER_FAULT_NONE] = "none",
    [TETHER_FAULT_CRASH] = "crash",
    [TETHER_FAULT_HANG] = "hang",
    [TETHER_FAULT_HOG] = "hog",
    [TETHER_FAULT_SLOW] = "slow",
    [TETHER_FAULT_NAN] = "nan",
    [TETHER_FAULT_SPIKE] = "spike",
    [TETHER_FAULT_DRIFT] = "drift",
    [TETHER_FAULT_STUCK] = "stuck",
    [TETHER_FAULT_BYPASS] = "bypass",
};

const char *tether_reason_name(tether_reason r)
{
    if ((unsigned)r >= TETHER_R_COUNT) return "unknown";
    return reason_names[r];
}

const char *tether_mode_name(tether_mode m)
{
    return m == TETHER_MODE_AI ? "AI" : "SAFE";
}

const char *tether_fault_name(tether_fault f)
{
    if ((unsigned)f >= TETHER_FAULT_COUNT) return "unknown";
    return fault_names[f];
}

tether_fault tether_fault_from_name(const char *name)
{
    if (name == NULL) return TETHER_FAULT_COUNT;
    for (int i = 0; i < TETHER_FAULT_COUNT; i++) {
        if (strcmp(name, fault_names[i]) == 0) return (tether_fault)i;
    }
    if (strcmp(name, "clear") == 0) return TETHER_FAULT_NONE;
    return TETHER_FAULT_COUNT;
}

int64_t tether_now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * TETHER_NS_PER_S + ts.tv_nsec;
}
