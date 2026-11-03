/*
 * Tether — the (untrusted) AI controller and its tooling.
 *
 * A small multilayer perceptron, evaluated from static storage with no
 * allocation so its run time does not depend on the heap. Weights are loaded
 * from a ".tmlp" file, which is what makes a model hot-swap possible: the
 * envelope stays fixed while the model behind it changes.
 *
 * .tmlp layout (little-endian):
 *   char[4] "TMLP"; u32 version(=1); u32 model_id; u32 n_in;
 *   f32 mean[n_in]; f32 std[n_in];
 *   u32 n_layers; per layer: u32 in, u32 out, u32 act(0 linear,1 tanh),
 *                            f32 w[out*in] (row-major), f32 b[out];
 *   f32 out_scale; f32 out_offset;
 * The output is driving pressure above PEEP: u = peep + out*scale + offset.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef TETHER_POLICY_H
#define TETHER_POLICY_H

#include <stddef.h>
#include <stdint.h>
#include "tether/arbiter.h"
#include "tether/common.h"

#ifdef __cplusplus
extern "C" {
#endif

#define TETHER_POLICY_NIN 8
#define TETHER_MLP_MAX_LAYERS 4
#define TETHER_MLP_MAX_WIDTH 64

typedef struct {
    uint32_t in, out, act;
    float w[TETHER_MLP_MAX_WIDTH * TETHER_MLP_MAX_WIDTH];
    float b[TETHER_MLP_MAX_WIDTH];
} tether_mlp_layer;

typedef struct {
    uint32_t model_id;
    uint32_t n_in, n_layers;
    float mean[TETHER_MLP_MAX_WIDTH], std[TETHER_MLP_MAX_WIDTH];
    tether_mlp_layer layer[TETHER_MLP_MAX_LAYERS];
    float out_scale, out_offset;
} tether_mlp;

int tether_mlp_load_mem(tether_mlp *m, const void *buf, size_t len, char *err, size_t errlen);
int tether_mlp_load_file(tether_mlp *m, const char *path, char *err, size_t errlen);
/* Raw network output (after out_scale/out_offset). */
double tether_mlp_eval(const tether_mlp *m, const float *x);

/* Feature vector the model sees; shared by training (via tether-datagen)
 * and inference so the two can never drift apart. */
void tether_policy_features(const tether_observation *o, float x[TETHER_POLICY_NIN]);
/* Absolute pressure command proposed by the model. */
double tether_policy_command(const tether_mlp *m, const tether_observation *o);

/* Privileged teacher used only to generate training data: it knows the
 * patient's true R and C, which no deployed controller does. Returns the
 * driving pressure above PEEP. */
double tether_teacher_dp(const tether_observation *o, double r, double c);
double tether_teacher_command(const tether_observation *o, double r, double c);

/* Value-level fault injection, applied to the model output in the AI
 * process. Process-level faults are handled by the process itself. */
typedef struct {
    tether_fault fault;
    uint32_t start_breath;
    double stuck_value;
    int stuck_set;
} tether_fault_state;

void tether_fault_arm(tether_fault_state *s, tether_fault f, uint32_t breath);
double tether_fault_apply(tether_fault_state *s, const tether_observation *o, double u);

#ifdef __cplusplus
}
#endif
#endif
