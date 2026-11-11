/*
 * Tether — wire protocol between the QNX processes.
 *
 * Message types live above _IO_MAX so they can share a channel with the
 * I/O messages that name_open()/open() generate. Every message starts with
 * tether_msg_hdr; replies are plain structs.
 *
 *   ai-ctrl  --MsgSend(PROPOSE)-->  arbiter     reply: tether_observation
 *   bridge   --MsgSend(STATUS)--->  arbiter     reply: tether_status
 *   bridge   --MsgSend(TELEMETRY)-> arbiter     reply: tether_telemetry_reply + samples
 *   bridge   --MsgSend(GUARD)---->  arbiter     reply: int32 (the new setting)
 *   bridge   --MsgSend(FAULT)---->  ai-ctrl     reply: tether_fault_reply
 *   bridge   --MsgSend(MODEL)---->  ai-ctrl     reply: tether_model_reply
 *   arbiter  --devctl------------>  /dev/tether/actuator, /dev/tether/sensor
 *   bridge   --devctl------------>  /dev/tether/plant  (scenario: change the lung)
 *
 * Direction is deliberate: the AI is always the client of the arbiter, never
 * its server, so nothing the AI does can leave the arbiter blocked.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef TETHER_PROTO_H
#define TETHER_PROTO_H

#include <stdint.h>
#include "tether/arbiter.h"
#include "tether/plant.h"
#include "tether/rt.h"

#ifdef __cplusplus
extern "C" {
#endif

#define TETHER_ARBITER_NAME "tether-arbiter"
#define TETHER_AI_NAME "tether-ai"
#define TETHER_DEV_DIR "/dev/tether"
#define TETHER_DEV_ACTUATOR "/dev/tether/actuator"
#define TETHER_DEV_SENSOR "/dev/tether/sensor"
#define TETHER_DEV_PLANT "/dev/tether/plant"

/* uid/gid the processes run as. The actuator node belongs to root and is
 * mode 0600; the AI runs as TETHER_UID_AI with every ability denied. */
#define TETHER_UID_AI 2002
#define TETHER_GID_AI 2002
#define TETHER_UID_BRIDGE 2003
#define TETHER_GID_OPS 2000

/* Above _IO_MAX (0x1ff) on QNX. */
enum {
    TETHER_MSG_PROPOSE = 0x5401,
    TETHER_MSG_STATUS,
    TETHER_MSG_TELEMETRY,
    TETHER_MSG_RESET_STATS,
    TETHER_MSG_FAULT,
    TETHER_MSG_MODEL,
    TETHER_MSG_AI_INFO,
    TETHER_MSG_GUARD
};

typedef struct {
    uint16_t type;
    uint16_t subtype;
    uint32_t pad;
} tether_msg_hdr;

typedef struct {
    tether_msg_hdr hdr;
    uint32_t seq;
    uint32_t model_id;
    double u;
    int64_t t_sent_ns;
} tether_msg_propose;

typedef struct {
    tether_msg_hdr hdr;
    uint64_t cursor;
    uint32_t max;
    uint32_t pad;
} tether_msg_telemetry;

#define TETHER_TELEMETRY_MAX 512
typedef struct {
    uint64_t next_cursor;
    uint32_t n;
    uint32_t pad;
    /* followed by n tether_sample */
} tether_telemetry_reply;

typedef struct {
    tether_msg_hdr hdr;
    int32_t fault;   /* tether_fault */
    int32_t pad;
} tether_msg_fault;

typedef struct {
    int64_t t_applied_ns;
    int32_t fault;
    int32_t status;  /* 0 or errno */
} tether_fault_reply;

/* bridge -> arbiter: 1 = runtime assurance on (normal), 0 = pass the AI straight
 * to the actuator. Demo only: it shows what the same AI does without Tether. */
typedef struct {
    tether_msg_hdr hdr;
    int32_t enable;
    int32_t pad;
} tether_msg_guard;

typedef struct {
    tether_msg_hdr hdr;
    char path[128];
} tether_msg_model;

typedef struct {
    int32_t status;
    uint32_t model_id;
    char err[120];
} tether_model_reply;

typedef struct {
    uint32_t model_id;
    int32_t fault;
    uint64_t proposals;
    uint64_t bypass_attempts, bypass_denied;
    int64_t infer_p99_ns, infer_max_ns;
    int32_t pid, uid;
} tether_ai_info;

/* ---- virtual hardware (devctl payloads) -------------------------------- */
typedef struct {
    double paw, flow;
    int64_t t_ns;
    uint64_t seq;
} tether_hw_sensor;

typedef struct {
    double u;
    int64_t t_ns;
} tether_hw_command;

typedef struct {
    tether_lung lung;
    double u_applied;
    uint64_t commands;
    uint32_t watchdog_trips, denied_opens;
    int32_t watchdog_active;
    int32_t pad;
} tether_hw_status;

#ifdef __QNXNTO__
#include <devctl.h>
/* /dev/tether/sensor   (0444): READ_SENSOR, HW_STATUS
 * /dev/tether/actuator (0600, root): SET_COMMAND
 * /dev/tether/plant    (0600, root): SET_LUNG, HW_STATUS */
#define DCMD_TETHER_READ_SENSOR __DIOF(_DCMD_MISC, 0x40, tether_hw_sensor)
#define DCMD_TETHER_SET_COMMAND __DIOT(_DCMD_MISC, 0x41, tether_hw_command)
#define DCMD_TETHER_HW_STATUS   __DIOF(_DCMD_MISC, 0x42, tether_hw_status)
#define DCMD_TETHER_SET_LUNG    __DIOT(_DCMD_MISC, 0x43, tether_lung)
#endif

#ifdef __cplusplus
}
#endif
#endif
