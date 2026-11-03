/*
 * Tether — runtime assurance for AI controllers on QNX.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Shared vocabulary: units, modes, the reasons the monitor gives for
 * taking control away from the AI, and the faults the demo can inject.
 *
 * Units used everywhere: pressure in cmH2O, volume in litres, flow in
 * litres per second, time in seconds (double) or nanoseconds (int64).
 */
#ifndef TETHER_COMMON_H
#define TETHER_COMMON_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TETHER_VERSION "0.1.0"

typedef enum {
    TETHER_MODE_SAFE = 0, /* verified recovery controller drives the actuator */
    TETHER_MODE_AI = 1    /* AI proposal accepted and applied */
} tether_mode;

/* Why a proposal was rejected (or why we are in SAFE mode). Order matters
 * only for display; values are part of the wire protocol. */
typedef enum {
    TETHER_OK = 0,
    TETHER_R_STARTUP,       /* monitor has not characterised the patient yet */
    TETHER_R_NO_PROPOSAL,   /* AI never proposed anything */
    TETHER_R_STALE,         /* last proposal older than the freshness deadline */
    TETHER_R_DISCONNECTED,  /* AI process went away (kernel disconnect pulse) */
    TETHER_R_NONFINITE,     /* NaN or Inf */
    TETHER_R_RANGE,         /* outside [p_min, p_max] */
    TETHER_R_RISE,          /* pressure rose faster than allowed */
    TETHER_R_EXP_PHASE,     /* pressure above PEEP during expiration */
    TETHER_R_PREDICT_VOL,   /* model lookahead says the lung would overdistend */
    TETHER_R_VOLUME_LIMIT,  /* delivered volume already at the limit */
    TETHER_R_MEASURED_PRESSURE, /* measured airway pressure above limit */
    TETHER_R_COUNT
} tether_reason;

const char *tether_reason_name(tether_reason r);
const char *tether_mode_name(tether_mode m);

/* Faults the demo can inject into the AI controller. Process-level faults
 * (crash, hang, hog, slow, bypass) are carried out by the AI process
 * itself; value-level faults are applied to the policy output. */
typedef enum {
    TETHER_FAULT_NONE = 0,
    TETHER_FAULT_CRASH,   /* write through a NULL pointer -> SIGSEGV */
    TETHER_FAULT_HANG,    /* spin forever, never propose again */
    TETHER_FAULT_HOG,     /* saturate every CPU at the AI's priority */
    TETHER_FAULT_SLOW,    /* inference takes 40 ms instead of < 1 ms */
    TETHER_FAULT_NAN,     /* output NaN */
    TETHER_FAULT_SPIKE,   /* output 70 cmH2O */
    TETHER_FAULT_DRIFT,   /* bias grows by 1 cmH2O every breath */
    TETHER_FAULT_STUCK,   /* repeat the last inspiratory value forever */
    TETHER_FAULT_BYPASS,  /* try to open the actuator directly */
    TETHER_FAULT_COUNT
} tether_fault;

const char *tether_fault_name(tether_fault f);
/* Returns TETHER_FAULT_COUNT if the name is unknown. */
tether_fault tether_fault_from_name(const char *name);

/* Monotonic clock in nanoseconds (CLOCK_MONOTONIC on both QNX and host). */
int64_t tether_now_ns(void);

#define TETHER_NS_PER_S 1000000000LL
#define TETHER_NS_PER_MS 1000000LL
#define TETHER_NS_PER_US 1000LL

#ifdef __cplusplus
}
#endif
#endif
