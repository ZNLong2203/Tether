/*
 * tether-bridge — HTTP/JSON gateway for the dashboard.
 *
 * Serves the dashboard page and a small API, and turns each request into the
 * message or devctl the corresponding Tether process understands. It runs as
 * the unprivileged operator user: it can change the lung and tell the AI to
 * misbehave, but it cannot open the actuator.
 *
 *   GET  /                    dashboard (www/index.html)
 *   GET  /api/status          arbiter status + hardware + AI info + model list
 *   GET  /api/telemetry?cursor=N   samples after N
 *   POST /api/fault?name=X    AI fault (crash hang hog slow nan spike drift stuck bypass none)
 *   POST /api/lung?preset=X   healthy | ards | asthma
 *   POST /api/guard?on=0|1    demo: pass the AI straight to the blower
 *   POST /api/model?name=X    hot-swap the AI's model (models/X.tmlp)
 *   POST /api/reset           start a new latency window
 *
 * Every call that talks to another process has a deadline, so a hung AI can
 * slow one request down but never wedge the gateway. There is no
 * authentication: this is a lab demo, reach it only through the forwarded
 * localhost port. POST needs the header "X-Tether: 1", which a web page on
 * another origin cannot add without a preflight we do not answer.
 * SPDX-License-Identifier: Apache-2.0
 */
#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/dispatch.h>
#include <sys/neutrino.h>
#include <sys/procmgr.h>
#include <sys/socket.h>
#include <unistd.h>

#include "qcommon.h"
#include "tether/common.h"
#include "tether/proto.h"
#include "tether/rt.h"

#define TAG "bridge"
#define CALL_TIMEOUT_NS (300 * TETHER_NS_PER_MS)

static const char *g_www = "/data/tether/www";
static const char *g_models = "/data/tether/models";
static tether_config g_cfg; /* same flags as the other processes: only control_hz is reported */

/* ---- talking to the other processes -------------------------------------- */

static long send_to(const char *name, const void *msg, size_t n, void *reply, size_t rn)
{
    int coid = name_open(name, 0);
    if (coid < 0) return -1;
    uint64_t to = CALL_TIMEOUT_NS;
    TimerTimeout(CLOCK_MONOTONIC, _NTO_TIMEOUT_SEND | _NTO_TIMEOUT_REPLY, NULL, &to, NULL);
    long r = MsgSend(coid, msg, n, reply, rn);
    name_close(coid);
    return r;
}

static int hw_status(tether_hw_status *st)
{
    static int fd = -1;
    for (int attempt = 0; attempt < 2; attempt++) {
        if (fd < 0) fd = open(TETHER_DEV_SENSOR, O_RDONLY);
        if (fd < 0) return -1;
        if (devctl(fd, DCMD_TETHER_HW_STATUS, st, sizeof(*st), NULL) == EOK) return 0;
        close(fd);
        fd = -1;
    }
    return -1;
}

/* ---- tiny HTTP helpers ------------------------------------------------------ */

static void respond(int fd, int code, const char *type, const char *body, size_t n)
{
    char h[256];
    const char *msg = code == 200 ? "OK" : code == 204 ? "No Content" : code == 400 ? "Bad Request" :
                      code == 403 ? "Forbidden" : code == 404 ? "Not Found" : "Error";
    int hn = snprintf(h, sizeof(h),
                      "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\n"
                      "Cache-Control: no-store\r\nConnection: close\r\n\r\n", code, msg, type, n);
    (void)!write(fd, h, (size_t)hn);
    size_t off = 0;
    while (off < n) {
        ssize_t w = write(fd, body + off, n - off);
        if (w <= 0) break;
        off += (size_t)w;
    }
}

static void respond_json(int fd, int code, const char *body)
{
    respond(fd, code, "application/json", body, strlen(body));
}

static int query_get(const char *q, const char *key, char *out, size_t n)
{
    size_t kl = strlen(key);
    while (q != NULL && *q) {
        if (strncmp(q, key, kl) == 0 && q[kl] == '=') {
            const char *v = q + kl + 1;
            size_t i = 0;
            while (v[i] && v[i] != '&' && i + 1 < n) {
                out[i] = v[i];
                i++;
            }
            out[i] = '\0';
            return 1;
        }
        q = strchr(q, '&');
        if (q) q++;
    }
    return 0;
}

static int safe_name(const char *s)
{
    size_t n = strlen(s);
    if (n == 0 || n > 64) return 0;
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.'))
            return 0;
    }
    return s[0] != '.';
}

/* ---- routes --------------------------------------------------------------------- */

static void route_index(int fd)
{
    char path[200];
    snprintf(path, sizeof(path), "%s/index.html", g_www);
    int f = open(path, O_RDONLY);
    if (f < 0) {
        respond(fd, 404, "text/plain", "dashboard not installed\n", 24);
        return;
    }
    static char page[262144];
    ssize_t n = read(f, page, sizeof(page));
    close(f);
    respond(fd, 200, "text/html; charset=utf-8", page, n > 0 ? (size_t)n : 0);
}

static int list_models(char *out, size_t n)
{
    size_t pos = 0;
    pos += (size_t)snprintf(out + pos, n - pos, "[");
    DIR *d = opendir(g_models);
    int first = 1;
    struct dirent *e;
    while (d != NULL && (e = readdir(d)) != NULL) {
        size_t l = strlen(e->d_name);
        if (l > 6 && strcmp(e->d_name + l - 5, ".tmlp") == 0 && pos + 100 < n) {
            pos += (size_t)snprintf(out + pos, n - pos, "%s\"%.*s\"", first ? "" : ",", (int)(l - 5), e->d_name);
            first = 0;
        }
    }
    if (d) closedir(d);
    pos += (size_t)snprintf(out + pos, n - pos, "]");
    return (int)pos;
}

static void route_status(int fd)
{
    static char body[20000], arb[8192], models[1024];
    static tether_status st;
    static int64_t ai_checked_ns;
    static tether_ai_info ai;
    static int have_ai;
    tether_msg_hdr h = { TETHER_MSG_STATUS, 0, 0 };
    if (send_to(TETHER_ARBITER_NAME, &h, sizeof(h), &st, sizeof(st)) != (long)sizeof(st) ||
        tether_status_json(&st, arb, sizeof(arb)) < 0) {
        respond_json(fd, 200, "{\"error\":\"arbiter not answering\"}");
        return;
    }
    int64_t now = tether_now_ns();
    if (now - ai_checked_ns > 1 * TETHER_NS_PER_S) { /* AI info once a second at most */
        tether_msg_hdr ih = { TETHER_MSG_AI_INFO, 0, 0 };
        have_ai = send_to(TETHER_AI_NAME, &ih, sizeof(ih), &ai, sizeof(ai)) == (long)sizeof(ai);
        ai_checked_ns = now;
    }
    tether_hw_status hw;
    int have_hw = hw_status(&hw) == 0;
    list_models(models, sizeof(models));
    int n = snprintf(body, sizeof(body), "{\"arbiter\":%s,", arb);
    if (have_hw)
        n += snprintf(body + n, sizeof(body) - (size_t)n,
                      "\"hw\":{\"lung_r\":%.1f,\"lung_c\":%.4f,\"u_applied\":%.2f,\"commands\":%llu,"
                      "\"watchdog_trips\":%u,\"watchdog_active\":%d,\"denied_opens\":%u},",
                      hw.lung.resistance, hw.lung.compliance, hw.u_applied, (unsigned long long)hw.commands,
                      hw.watchdog_trips, hw.watchdog_active, hw.denied_opens);
    if (have_ai)
        n += snprintf(body + n, sizeof(body) - (size_t)n,
                      "\"ai\":{\"model_id\":%u,\"fault\":\"%s\",\"proposals\":%llu,\"bypass_attempts\":%llu,"
                      "\"bypass_denied\":%llu,\"infer_p99_us\":%lld,\"infer_max_us\":%lld,\"pid\":%d,\"uid\":%d},",
                      ai.model_id, tether_fault_name((tether_fault)ai.fault), (unsigned long long)ai.proposals,
                      (unsigned long long)ai.bypass_attempts, (unsigned long long)ai.bypass_denied,
                      (long long)(ai.infer_p99_ns / 1000), (long long)(ai.infer_max_ns / 1000), ai.pid, ai.uid);
    n += snprintf(body + n, sizeof(body) - (size_t)n, "\"control_hz\":%.0f,\"models\":%s,\"version\":\"%s\"}",
                  g_cfg.timing.control_hz, models, TETHER_VERSION);
    respond_json(fd, 200, body);
}

static void route_telemetry(int fd, const char *query)
{
    static struct {
        tether_telemetry_reply h;
        tether_sample s[TETHER_TELEMETRY_MAX];
    } r;
    static char body[110000];
    char v[24];
    tether_msg_telemetry m = { { TETHER_MSG_TELEMETRY, 0, 0 }, 0, TETHER_TELEMETRY_MAX, 0 };
    if (query_get(query, "cursor", v, sizeof(v))) m.cursor = strtoull(v, NULL, 10);
    long n = send_to(TETHER_ARBITER_NAME, &m, sizeof(m), &r, sizeof(r));
    if (n < (long)sizeof(r.h)) {
        respond_json(fd, 200, "{\"error\":\"arbiter not answering\"}");
        return;
    }
    int off = snprintf(body, sizeof(body), "{\"next\":%llu,\"fmt\":\"tick,paw,flow,vol,u,u_ai,mode,verdict,phase,violation,breath,wake_ns\","
                       "\"samples\":", (unsigned long long)r.h.next_cursor);
    int j = tether_samples_json(r.s, r.h.n, 1, body + off, sizeof(body) - (size_t)off - 4);
    if (j < 0) {
        respond_json(fd, 200, "{\"error\":\"too many samples\"}");
        return;
    }
    memcpy(body + off + j, "}", 2);
    respond_json(fd, 200, body);
}

static void route_post(int fd, const char *path, const char *query)
{
    char v[96], out[256];
    if (strcmp(path, "/api/fault") == 0 && query_get(query, "name", v, sizeof(v))) {
        tether_fault f = tether_fault_from_name(v);
        if (f == TETHER_FAULT_COUNT) {
            respond_json(fd, 400, "{\"ok\":false,\"error\":\"unknown fault\"}");
            return;
        }
        tether_msg_fault m = { { TETHER_MSG_FAULT, 0, 0 }, (int32_t)f, 0 };
        tether_fault_reply rep;
        long r = send_to(TETHER_AI_NAME, &m, sizeof(m), &rep, sizeof(rep));
        snprintf(out, sizeof(out), "{\"ok\":%s,\"fault\":\"%s\"%s}", r == (long)sizeof(rep) && !rep.status ? "true" : "false",
                 tether_fault_name(f), r == (long)sizeof(rep) ? "" : ",\"error\":\"the AI process did not answer\"");
        respond_json(fd, 200, out);
    } else if (strcmp(path, "/api/lung") == 0 && query_get(query, "preset", v, sizeof(v))) {
        tether_lung l;
        if (tether_lung_preset(&l, v) != 0) {
            respond_json(fd, 400, "{\"ok\":false,\"error\":\"unknown preset\"}");
            return;
        }
        int dev = open(TETHER_DEV_PLANT, O_WRONLY);
        int e = dev < 0 ? errno : devctl(dev, DCMD_TETHER_SET_LUNG, &l, sizeof(l), NULL);
        if (dev >= 0) close(dev);
        if (e == EOK) {
            respond_json(fd, 200, "{\"ok\":true}");
        } else {
            snprintf(out, sizeof(out), "{\"ok\":false,\"error\":\"%s\"}", strerror(e));
            respond_json(fd, 500, out);
        }
    } else if (strcmp(path, "/api/guard") == 0 && query_get(query, "on", v, sizeof(v))) {
        tether_msg_guard m = { { TETHER_MSG_GUARD, 0, 0 }, v[0] == '1', 0 };
        int32_t now_on = 0;
        long r = send_to(TETHER_ARBITER_NAME, &m, sizeof(m), &now_on, sizeof(now_on));
        snprintf(out, sizeof(out), "{\"ok\":%s,\"guard\":%s}", r == (long)sizeof(now_on) ? "true" : "false", now_on ? "true" : "false");
        respond_json(fd, 200, out);
    } else if (strcmp(path, "/api/reset") == 0) {
        tether_msg_hdr h = { TETHER_MSG_RESET_STATS, 0, 0 };
        int32_t ok = 0;
        long r = send_to(TETHER_ARBITER_NAME, &h, sizeof(h), &ok, sizeof(ok));
        respond_json(fd, 200, r == (long)sizeof(ok) ? "{\"ok\":true}" : "{\"ok\":false}");
    } else if (strcmp(path, "/api/model") == 0 && query_get(query, "name", v, sizeof(v))) {
        if (!safe_name(v)) {
            respond_json(fd, 400, "{\"ok\":false,\"error\":\"bad model name\"}");
            return;
        }
        tether_msg_model m;
        memset(&m, 0, sizeof(m));
        m.hdr.type = TETHER_MSG_MODEL;
        snprintf(m.path, sizeof(m.path), "%s/%s.tmlp", g_models, v);
        tether_model_reply rep;
        long r = send_to(TETHER_AI_NAME, &m, sizeof(m), &rep, sizeof(rep));
        if (r != (long)sizeof(rep)) {
            respond_json(fd, 200, "{\"ok\":false,\"error\":\"the AI process did not answer\"}");
        } else {
            for (char *p = rep.err; *p; p++) if (*p == '"' || *p == '\\' || *p < 32) *p = ' ';
            snprintf(out, sizeof(out), "{\"ok\":%s,\"model_id\":%u,\"error\":\"%s\"}", rep.status ? "false" : "true", rep.model_id, rep.err);
            respond_json(fd, 200, out);
        }
    } else {
        respond_json(fd, 404, "{\"ok\":false,\"error\":\"no such call\"}");
    }
}

static void handle(int fd)
{
    char req[4096];
    size_t n = 0;
    while (n < sizeof(req) - 1) {
        ssize_t r = read(fd, req + n, sizeof(req) - 1 - n);
        if (r <= 0) break;
        n += (size_t)r;
        req[n] = '\0';
        if (strstr(req, "\r\n\r\n") != NULL) break;
    }
    req[n] = '\0';
    char method[8] = "", target[512] = "";
    if (sscanf(req, "%7s %511s", method, target) != 2) {
        respond(fd, 400, "text/plain", "bad request\n", 12);
        return;
    }
    char *q = strchr(target, '?');
    if (q) *q++ = '\0';

    if (strcmp(method, "GET") == 0) {
        if (strcmp(target, "/") == 0 || strcmp(target, "/index.html") == 0) route_index(fd);
        else if (strcmp(target, "/api/status") == 0) route_status(fd);
        else if (strcmp(target, "/api/telemetry") == 0) route_telemetry(fd, q);
        else if (strcmp(target, "/favicon.ico") == 0) respond(fd, 204, "text/plain", "", 0);
        else respond(fd, 404, "text/plain", "not found\n", 10);
    } else if (strcmp(method, "POST") == 0) {
        if (strstr(req, "X-Tether: 1") == NULL && strstr(req, "x-tether: 1") == NULL) {
            respond_json(fd, 403, "{\"ok\":false,\"error\":\"missing X-Tether header\"}");
            return;
        }
        route_post(fd, target, q);
    } else {
        respond(fd, 400, "text/plain", "bad request\n", 12);
    }
}

static void drop_privileges(void)
{
    int e = procmgr_ability(0,
                            PROCMGR_ADN_NONROOT | PROCMGR_AOP_DENY | PROCMGR_AOP_LOCK | PROCMGR_AID_SPAWN,
                            PROCMGR_ADN_NONROOT | PROCMGR_AOP_DENY | PROCMGR_AOP_LOCK | PROCMGR_AID_FORK,
                            PROCMGR_ADN_NONROOT | PROCMGR_AOP_DENY | PROCMGR_AOP_LOCK | PROCMGR_AID_SETUID,
                            PROCMGR_ADN_NONROOT | PROCMGR_AOP_DENY | PROCMGR_AOP_LOCK | PROCMGR_AID_SETGID,
                            PROCMGR_ADN_NONROOT | PROCMGR_AOP_DENY | PROCMGR_AOP_LOCK | PROCMGR_AID_MEM_PHYS,
                            PROCMGR_ADN_NONROOT | PROCMGR_AOP_DENY | PROCMGR_AOP_LOCK | PROCMGR_AID_INTERRUPT,
                            PROCMGR_ADN_NONROOT | PROCMGR_AOP_DENY | PROCMGR_AOP_LOCK | PROCMGR_AID_IO,
                            PROCMGR_ADN_NONROOT | PROCMGR_AOP_DENY | PROCMGR_AOP_LOCK | PROCMGR_AID_REBOOT,
                            PROCMGR_AID_EOL);
    if (e != EOK) qlog(TAG, "procmgr_ability failed: %s", strerror(e));
    if (setgid(TETHER_GID_OPS) != 0 || setuid(TETHER_UID_BRIDGE) != 0) {
        qlog(TAG, "cannot drop privileges: %s; refusing to serve as root", strerror(errno));
        exit(1);
    }
    qlog(TAG, "serving as uid %d gid %d", (int)geteuid(), (int)getegid());
}

int main(int argc, char **argv)
{
    int port = 8080, keep_root = 0;
    tether_config_defaults(&g_cfg);
    for (int i = 1; i < argc; i++) {
        if (q_config_arg(argv[i], &g_cfg)) continue;
        if (strncmp(argv[i], "--port=", 7) == 0) port = atoi(argv[i] + 7);
        else if (strncmp(argv[i], "--www=", 6) == 0) g_www = argv[i] + 6;
        else if (strncmp(argv[i], "--models=", 9) == 0) g_models = argv[i] + 9;
        else if (strcmp(argv[i], "--keep-root") == 0) keep_root = 1;
    }
    int ls = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    a.sin_port = htons((uint16_t)port);
    if (bind(ls, (struct sockaddr *)&a, sizeof(a)) != 0 || listen(ls, 8) != 0) {
        qlog(TAG, "cannot listen on port %d: %s", port, strerror(errno));
        return 1;
    }
    if (!keep_root) drop_privileges();
    q_set_fifo(TETHER_PRIO_BRIDGE);
    qlog(TAG, "listening on port %d", port);

    for (;;) {
        int fd = accept(ls, NULL, NULL);
        if (fd < 0) continue;
        struct timeval tv = { 2, 0 }; /* a slow client must not hold the gateway */
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        handle(fd);
        close(fd);
    }
}
