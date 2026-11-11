/* SPDX-License-Identifier: Apache-2.0 */
#include "tether/rt.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* ---- histogram ---------------------------------------------------------- */

void tether_hist_reset(tether_hist *h)
{
    memset(h, 0, sizeof(*h));
    h->min_ns = INT64_MAX;
}

static int bucket_of(int64_t ns)
{
    if (ns < 0) ns = 0;
    int64_t us = ns / 1000;
    if (us < 1000) return (int)us;                            /* 0..999     */
    if (us < 10000) return 1000 + (int)((us - 1000) / 10);    /* 1000..1899 */
    if (us < 100000) return 1900 + (int)((us - 10000) / 100); /* 1900..2799 */
    return TETHER_HIST_BUCKETS - 1;
}

static int64_t bucket_upper_ns(int b)
{
    if (b < 1000) return (int64_t)(b + 1) * 1000;
    if (b < 1900) return (int64_t)(1000 + (b - 1000 + 1) * 10) * 1000;
    if (b < TETHER_HIST_BUCKETS - 1) return (int64_t)(10000 + (b - 1900 + 1) * 100) * 1000;
    return INT64_MAX;
}

void tether_hist_add(tether_hist *h, int64_t ns)
{
    int b = bucket_of(ns);
    if (b > TETHER_HIST_BUCKETS - 1) b = TETHER_HIST_BUCKETS - 1;
    h->bucket[b]++;
    h->count++;
    h->sum_ns += (double)ns;
    if (ns < h->min_ns) h->min_ns = ns;
    if (ns > h->max_ns) h->max_ns = ns;
}

int64_t tether_hist_percentile(const tether_hist *h, double p)
{
    if (h->count == 0) return 0;
    uint64_t want = (uint64_t)ceil((p / 100.0) * (double)h->count);
    if (want < 1) want = 1;
    uint64_t acc = 0;
    for (int b = 0; b < TETHER_HIST_BUCKETS; b++) {
        acc += h->bucket[b];
        if (acc >= want) {
            int64_t up = bucket_upper_ns(b);
            return up > h->max_ns ? h->max_ns : up;
        }
    }
    return h->max_ns;
}

/* ---- ring ---------------------------------------------------------------- */

void tether_ring_init(tether_ring *r)
{
    atomic_store_explicit(&r->head, 0, memory_order_relaxed);
}

void tether_ring_push(tether_ring *r, const tether_sample *s)
{
    uint64_t h = atomic_load_explicit(&r->head, memory_order_relaxed);
    r->s[h % TETHER_RING_CAP] = *s;
    atomic_store_explicit(&r->head, h + 1, memory_order_release);
}

size_t tether_ring_read(tether_ring *r, uint64_t *cursor, tether_sample *out, size_t max)
{
    uint64_t head = atomic_load_explicit(&r->head, memory_order_acquire);
    uint64_t oldest = head > TETHER_RING_CAP - 1 ? head - (TETHER_RING_CAP - 1) : 0;
    if (*cursor < oldest) *cursor = oldest;
    if (*cursor > head) *cursor = head;
    size_t n = (size_t)(head - *cursor);
    if (n > max) n = max;
    for (size_t i = 0; i < n; i++) out[i] = r->s[(*cursor + i) % TETHER_RING_CAP];
    /* Anything the writer may have overwritten while we copied is dropped. */
    atomic_thread_fence(memory_order_acquire);
    uint64_t head2 = atomic_load_explicit(&r->head, memory_order_relaxed);
    uint64_t safe_from = head2 > TETHER_RING_CAP - 1 ? head2 - (TETHER_RING_CAP - 1) : 0;
    size_t skip = 0;
    if (safe_from > *cursor) {
        skip = (size_t)(safe_from - *cursor);
        if (skip > n) skip = n;
        memmove(out, out + skip, (n - skip) * sizeof(*out));
    }
    *cursor += n;
    return n - skip;
}

/* ---- mailbox ------------------------------------------------------------ */

void tether_mailbox_init(tether_mailbox *m)
{
    memset(&m->p, 0, sizeof(m->p));
    atomic_store_explicit(&m->seq, 0, memory_order_relaxed);
}

void tether_mailbox_write(tether_mailbox *m, const tether_proposal *p)
{
    uint32_t s = atomic_load_explicit(&m->seq, memory_order_relaxed);
    atomic_store_explicit(&m->seq, s + 1, memory_order_relaxed);
    atomic_thread_fence(memory_order_release);
    m->p = *p;
    atomic_store_explicit(&m->seq, s + 2, memory_order_release);
}

int tether_mailbox_read(tether_mailbox *m, tether_proposal *out)
{
    for (int attempt = 0; attempt < 3; attempt++) {
        uint32_t s1 = atomic_load_explicit(&m->seq, memory_order_acquire);
        if (s1 & 1u) continue;
        tether_proposal tmp = m->p;
        atomic_thread_fence(memory_order_acquire);
        uint32_t s2 = atomic_load_explicit(&m->seq, memory_order_relaxed);
        if (s1 == s2) {
            *out = tmp;
            return 1;
        }
    }
    return 0;
}

/* ---- status ------------------------------------------------------------- */

void tether_eventlog_push(tether_eventlog *l, const tether_switch_event *e)
{
    l->ev[l->seq % TETHER_EVENT_LOG] = *e;
    l->seq++;
}

void tether_status_fill(tether_status *s, const tether_arbiter *a, const tether_hist *wake,
                        const tether_hist *exec, const tether_eventlog *log,
                        const tether_proposal *last, int64_t now_ns, int ai_connected)
{
    memset(s, 0, sizeof(*s));
    s->tick = a->tick;
    s->t_ns = now_ns;
    s->mode = a->mode;
    s->reason = a->reason;
    s->breath = a->breath;
    s->phase = a->phase;
    s->vt_last = a->vt_last;
    s->dp_last = a->dp_last;
    s->vt_target = a->cfg.vent.vt_target;
    s->vt_max = a->cfg.env.vt_max;
    s->p_max = a->cfg.env.p_max;
    s->peep = a->cfg.vent.peep;
    s->est_r = a->est.r;
    s->est_c = a->est.c;
    s->est_valid = a->est.valid;
    s->ai_connected = ai_connected;
    if (last != NULL && last->valid) {
        s->ai_model_id = last->model_id;
        s->ai_seq = last->seq;
        s->ai_age_ns = now_ns - last->recv_ns;
    } else {
        s->ai_age_ns = -1;
    }
    s->ticks_ai = a->ticks_ai;
    s->ticks_safe = a->ticks_safe;
    s->switches_to_safe = a->switches_to_safe;
    s->readmissions = a->readmissions;
    s->violations = a->violations;
    memcpy(s->reason_count, a->reason_count, sizeof(s->reason_count));
    if (wake != NULL) {
        s->wake_count = wake->count;
        s->wake_p50_ns = tether_hist_percentile(wake, 50.0);
        s->wake_p99_ns = tether_hist_percentile(wake, 99.0);
        s->wake_p999_ns = tether_hist_percentile(wake, 99.9);
        s->wake_max_ns = wake->max_ns;
    }
    if (exec != NULL) {
        s->exec_p99_ns = tether_hist_percentile(exec, 99.0);
        s->exec_max_ns = exec->max_ns;
    }
    if (log != NULL) {
        s->event_seq = log->seq;
        uint64_t n = log->seq < TETHER_EVENT_LOG ? log->seq : TETHER_EVENT_LOG;
        for (uint64_t i = 0; i < n; i++)
            s->events[i] = log->ev[(log->seq - 1 - i) % TETHER_EVENT_LOG];
    }
}

/* ---- JSON --------------------------------------------------------------- */

typedef struct {
    char *buf;
    size_t len, pos;
    int overflow;
} jbuf;

static void jp(jbuf *j, const char *fmt, ...)
{
    if (j->overflow) return;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(j->buf + j->pos, j->len - j->pos, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= j->len - j->pos) {
        j->overflow = 1;
        return;
    }
    j->pos += (size_t)n;
}

/* JSON has no NaN/Inf: emit null. */
static void jnum(jbuf *j, const char *key, double v, int prec)
{
    if (isfinite(v)) jp(j, "\"%s\":%.*f", key, prec, v);
    else jp(j, "\"%s\":null", key);
}

int tether_status_json(const tether_status *s, char *buf, size_t len)
{
    jbuf j = { buf, len, 0, 0 };
    jp(&j, "{\"tick\":%llu,\"t_ns\":%lld,\"mode\":\"%s\",\"reason\":\"%s\",\"breath\":%u,\"phase\":%d,",
       (unsigned long long)s->tick, (long long)s->t_ns, tether_mode_name((tether_mode)s->mode),
       tether_reason_name((tether_reason)s->reason), s->breath, s->phase);
    jnum(&j, "vt_last", s->vt_last, 4); jp(&j, ",");
    jnum(&j, "dp_last", s->dp_last, 2); jp(&j, ",");
    jnum(&j, "vt_target", s->vt_target, 3); jp(&j, ",");
    jnum(&j, "vt_max", s->vt_max, 3); jp(&j, ",");
    jnum(&j, "p_max", s->p_max, 1); jp(&j, ",");
    jnum(&j, "peep", s->peep, 1); jp(&j, ",");
    jnum(&j, "est_r", s->est_r, 2); jp(&j, ",");
    jnum(&j, "est_c", s->est_c, 4); jp(&j, ",");
    jp(&j, "\"est_valid\":%d,\"ai_connected\":%d,\"ai_model_id\":%u,\"ai_seq\":%u,\"ai_age_ns\":%lld,",
       s->est_valid, s->ai_connected, s->ai_model_id, s->ai_seq, (long long)s->ai_age_ns);
    jp(&j, "\"ticks_ai\":%llu,\"ticks_safe\":%llu,\"switches_to_safe\":%llu,\"readmissions\":%llu,\"violations\":%llu,",
       (unsigned long long)s->ticks_ai, (unsigned long long)s->ticks_safe,
       (unsigned long long)s->switches_to_safe, (unsigned long long)s->readmissions,
       (unsigned long long)s->violations);
    jp(&j, "\"reason_count\":{");
    for (int r = 1; r < TETHER_R_COUNT; r++)
        jp(&j, "%s\"%s\":%llu", r > 1 ? "," : "", tether_reason_name((tether_reason)r),
           (unsigned long long)s->reason_count[r]);
    jp(&j, "},\"wake\":{\"count\":%llu,\"p50_ns\":%lld,\"p99_ns\":%lld,\"p999_ns\":%lld,\"max_ns\":%lld},",
       (unsigned long long)s->wake_count, (long long)s->wake_p50_ns, (long long)s->wake_p99_ns,
       (long long)s->wake_p999_ns, (long long)s->wake_max_ns);
    jp(&j, "\"exec\":{\"p99_ns\":%lld,\"max_ns\":%lld},", (long long)s->exec_p99_ns, (long long)s->exec_max_ns);
    jp(&j, "\"ctl_overruns\":%llu,\"guard_off\":%u,\"ai_restarts\":%u,\"hw_watchdog_trips\":%u,\"hw_denied_opens\":%u,\"event_seq\":%llu,\"events\":[",
       (unsigned long long)s->ctl_overruns, s->guard_off, s->ai_restarts, s->hw_watchdog_trips, s->hw_denied_opens, (unsigned long long)s->event_seq);
    uint64_t n = s->event_seq < TETHER_EVENT_LOG ? s->event_seq : TETHER_EVENT_LOG;
    for (uint64_t i = 0; i < n; i++) {
        const tether_switch_event *e = &s->events[i];
        jp(&j, "%s{\"seq\":%llu,\"t_ns\":%lld,\"tick\":%llu,\"to\":\"%s\",\"reason\":\"%s\",\"breath\":%u}",
           i ? "," : "", (unsigned long long)(s->event_seq - i), (long long)e->t_ns,
           (unsigned long long)e->tick, tether_mode_name((tether_mode)e->to_mode),
           tether_reason_name((tether_reason)e->reason), e->breath);
    }
    jp(&j, "]}");
    return j.overflow ? -1 : (int)j.pos;
}

int tether_samples_json(const tether_sample *s, size_t n, size_t stride, char *buf, size_t len)
{
    jbuf j = { buf, len, 0, 0 };
    if (stride == 0) stride = 1;
    jp(&j, "[");
    int first = 1;
    for (size_t i = 0; i < n; i += stride) {
        const tether_sample *x = &s[i];
        jp(&j, "%s[%llu,%.2f,%.4f,%.4f,%.2f,", first ? "" : ",", (unsigned long long)x->tick,
           (double)x->paw, (double)x->flow, (double)x->vol, (double)x->u);
        if (isfinite(x->u_ai)) jp(&j, "%.2f", (double)x->u_ai);
        else jp(&j, "null");
        jp(&j, ",%d,%d,%d,%d,%u,%d]", x->mode, x->verdict, x->phase, x->violation, x->breath, x->wake_ns);
        first = 0;
    }
    jp(&j, "]");
    return j.overflow ? -1 : (int)j.pos;
}
