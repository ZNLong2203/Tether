/* SPDX-License-Identifier: Apache-2.0 */
#include "tether/policy.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fail(char *err, size_t errlen, const char *fmt, ...)
{
    if (err != NULL && errlen > 0) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(err, errlen, fmt, ap);
        va_end(ap);
    }
    return -1;
}

typedef struct {
    const uint8_t *p;
    size_t left;
} reader;

static int rd_u32(reader *r, uint32_t *v)
{
    if (r->left < 4) return -1;
    *v = (uint32_t)r->p[0] | ((uint32_t)r->p[1] << 8) | ((uint32_t)r->p[2] << 16) | ((uint32_t)r->p[3] << 24);
    r->p += 4;
    r->left -= 4;
    return 0;
}

static int rd_f32(reader *r, float *v)
{
    uint32_t u;
    if (rd_u32(r, &u) != 0) return -1;
    memcpy(v, &u, sizeof(*v));
    return 0;
}

int tether_mlp_load_mem(tether_mlp *m, const void *buf, size_t len, char *err, size_t errlen)
{
    reader r = { (const uint8_t *)buf, len };
    uint32_t version;
    memset(m, 0, sizeof(*m));
    if (len < 4 || memcmp(buf, "TMLP", 4) != 0) return fail(err, errlen, "not a .tmlp file");
    r.p += 4;
    r.left -= 4;
    if (rd_u32(&r, &version) || version != 1) return fail(err, errlen, "unsupported .tmlp version");
    if (rd_u32(&r, &m->model_id) || rd_u32(&r, &m->n_in)) return fail(err, errlen, "truncated header");
    if (m->n_in != TETHER_POLICY_NIN) return fail(err, errlen, "model expects %u inputs, policy has %d", m->n_in, TETHER_POLICY_NIN);
    for (uint32_t i = 0; i < m->n_in; i++)
        if (rd_f32(&r, &m->mean[i])) return fail(err, errlen, "truncated normaliser");
    for (uint32_t i = 0; i < m->n_in; i++) {
        if (rd_f32(&r, &m->std[i])) return fail(err, errlen, "truncated normaliser");
        if (!(m->std[i] > 0.0f)) return fail(err, errlen, "std[%u] must be positive", i);
    }
    if (rd_u32(&r, &m->n_layers) || m->n_layers == 0 || m->n_layers > TETHER_MLP_MAX_LAYERS)
        return fail(err, errlen, "bad layer count");
    uint32_t prev = m->n_in;
    for (uint32_t l = 0; l < m->n_layers; l++) {
        tether_mlp_layer *L = &m->layer[l];
        if (rd_u32(&r, &L->in) || rd_u32(&r, &L->out) || rd_u32(&r, &L->act))
            return fail(err, errlen, "truncated layer %u", l);
        if (L->in != prev || L->out == 0 || L->out > TETHER_MLP_MAX_WIDTH || L->act > 1)
            return fail(err, errlen, "bad shape in layer %u", l);
        for (uint32_t i = 0; i < L->in * L->out; i++)
            if (rd_f32(&r, &L->w[i])) return fail(err, errlen, "truncated weights in layer %u", l);
        for (uint32_t i = 0; i < L->out; i++)
            if (rd_f32(&r, &L->b[i])) return fail(err, errlen, "truncated bias in layer %u", l);
        prev = L->out;
    }
    if (prev != 1) return fail(err, errlen, "model must have a single output");
    if (rd_f32(&r, &m->out_scale) || rd_f32(&r, &m->out_offset)) return fail(err, errlen, "truncated output scale");
    return 0;
}

int tether_mlp_load_file(tether_mlp *m, const char *path, char *err, size_t errlen)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL) return fail(err, errlen, "cannot open %s", path);
    static uint8_t buf[sizeof(tether_mlp) + 4096];
    size_t n = fread(buf, 1, sizeof(buf), f);
    fclose(f);
    return tether_mlp_load_mem(m, buf, n, err, errlen);
}

double tether_mlp_eval(const tether_mlp *m, const float *x)
{
    float a[TETHER_MLP_MAX_WIDTH], b[TETHER_MLP_MAX_WIDTH];
    for (uint32_t i = 0; i < m->n_in; i++) a[i] = (x[i] - m->mean[i]) / m->std[i];
    float *in = a, *out = b;
    for (uint32_t l = 0; l < m->n_layers; l++) {
        const tether_mlp_layer *L = &m->layer[l];
        for (uint32_t o = 0; o < L->out; o++) {
            float s = L->b[o];
            const float *w = &L->w[o * L->in];
            for (uint32_t i = 0; i < L->in; i++) s += w[i] * in[i];
            out[o] = L->act == 1 ? tanhf(s) : s;
        }
        float *t = in;
        in = out;
        out = t;
    }
    return (double)in[0] * m->out_scale + m->out_offset;
}

void tether_policy_features(const tether_observation *o, float x[TETHER_POLICY_NIN])
{
    double tp = o->ti > 0 ? o->t_in_phase / o->ti : 0.0;
    if (tp > 3.0) tp = 3.0;
    x[0] = (float)o->phase;
    x[1] = (float)tp;
    x[2] = (float)(o->paw - o->peep);
    x[3] = (float)o->flow;
    x[4] = (float)o->vol;
    x[5] = (float)o->vt_target;
    /* Mechanics come from the arbiter's sensor-only estimator, not from the
     * last breath's applied pressure: that would make the model's own output
     * (or the recovery controller's) an input to its next decision. */
    x[6] = (float)o->est_c;
    x[7] = (float)o->est_r;
}

double tether_policy_command(const tether_mlp *m, const tether_observation *o)
{
    float x[TETHER_POLICY_NIN];
    tether_policy_features(o, x);
    return o->peep + tether_mlp_eval(m, x);
}

double tether_teacher_dp(const tether_observation *o, double r, double c)
{
    if (o->phase != 0) return 0.0;
    double tau = r * c;
    double reach = 1.0 - exp(-(o->ti - 0.5 * o->rise_time) / tau);
    if (reach < 0.2) reach = 0.2;
    double dp = o->vt_target / (c * reach);
    if (dp < 2.0) dp = 2.0;
    if (dp > 25.0) dp = 25.0;
    double ramp = o->t_in_phase >= o->rise_time ? 1.0 : o->t_in_phase / o->rise_time;
    return dp * ramp;
}

double tether_teacher_command(const tether_observation *o, double r, double c)
{
    return o->peep + tether_teacher_dp(o, r, c);
}

void tether_fault_arm(tether_fault_state *s, tether_fault f, uint32_t breath)
{
    s->fault = f;
    s->start_breath = breath;
    s->stuck_set = 0;
    s->stuck_value = 0.0;
}

double tether_fault_apply(tether_fault_state *s, const tether_observation *o, double u)
{
    switch (s->fault) {
    case TETHER_FAULT_NAN:
        return NAN;
    case TETHER_FAULT_SPIKE:
        return 70.0;
    case TETHER_FAULT_DRIFT: {
        /* A slowly growing bias: plausible-looking, never out of range at
         * first, and only caught by the model lookahead. */
        uint32_t breaths = o->breath >= s->start_breath ? o->breath - s->start_breath : 0;
        return o->phase == 0 ? u + 1.0 * (breaths + 1) : u;
    }
    case TETHER_FAULT_STUCK:
        /* Latch an inspiratory value and keep sending it into expiration:
         * the patient could no longer breathe out. */
        if (!s->stuck_set && o->phase == 0 && o->t_in_phase > o->rise_time) {
            s->stuck_value = u;
            s->stuck_set = 1;
        }
        return s->stuck_set ? s->stuck_value : u;
    default:
        return u;
    }
}
