/*
 * Shared world state, the text command API and JSON telemetry.
 *
 * The lock is a priority-inheritance mutex: if the prio-60 guardian ever
 * blocks on it while the prio-15 HTTP thread holds it, the HTTP thread is
 * boosted to 60 until it lets go. (QNX mutexes inherit priority by default;
 * we also ask for it explicitly so the intent is visible.)
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "cc.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/utsname.h>

cc_world_t W;
static double g_t0;

void cc_world_init(void)
{
    pthread_mutexattr_t ma;
    struct utsname u;

    memset(&W, 0, sizeof W);
    pthread_mutexattr_init(&ma);
    pthread_mutexattr_setprotocol(&ma, PTHREAD_PRIO_INHERIT);
    pthread_mutex_init(&W.lock, &ma);
    pthread_mutexattr_destroy(&ma);

    g_t0 = cc_now_s();
    W.s.temp_c      = 4.6;
    W.t_true        = 4.6;
    W.s.ambient_c   = 32.0;        /* a Kano afternoon */
    W.s.grid_power  = 1;
    W.s.battery_pct = 100.0;
    W.g.ttb_s       = -1;
    W.sim_speed     = 1.0;
    cc_timing_reset(&W.g.timing);
    cc_timing_reset(&W.g.naive);

#ifdef __QNX__
    W.qnx_native = 1;
#endif
    if (uname(&u) == 0)
        snprintf(W.platform, sizeof W.platform, "%s %s %s", u.sysname, u.release, u.machine);
    else
        snprintf(W.platform, sizeof W.platform, "unknown");
}

/* caller may or may not hold the lock — journal has its own tiny lock */
static pthread_mutex_t ev_lock = PTHREAD_MUTEX_INITIALIZER;

void cc_event(char level, const char *fmt, ...)
{
    va_list ap;
    cc_event_t *e;

    pthread_mutex_lock(&ev_lock);
    e = &W.ev[W.ev_head % CC_EVENTS];
    e->t = cc_now_s() - g_t0;
    e->level = level;
    va_start(ap, fmt);
    vsnprintf(e->msg, sizeof e->msg, fmt, ap);
    va_end(ap);
    W.ev_head++;
    pthread_mutex_unlock(&ev_lock);
    fprintf(stderr, "[%8.2f] %c %s\n", e->t, level, e->msg);
}

static int is(const char *a, const char *b) { return strcmp(a, b) == 0; }

/*
 * Text command interface. The same commands arrive from three places:
 *   echo door_open > /dev/coldchain        (resource manager write)
 *   POST /api/cmd?do=door_open             (dashboard)
 *   the guardian itself (backup_on / backup_off)
 */
int cc_command(const char *in, char *reply, int replylen)
{
    char cmd[48];
    int n = 0, ok = 1;

    while (*in == ' ' || *in == '\t') in++;
    while (in[n] && in[n] != '\n' && in[n] != '\r' && in[n] != ' ' && n < (int)sizeof cmd - 1) {
        cmd[n] = in[n];
        n++;
    }
    cmd[n] = 0;

    pthread_mutex_lock(&W.lock);
    if      (is(cmd, "door_open"))    { W.s.door_open = 1; }
    else if (is(cmd, "door_close"))   { W.s.door_open = 0; }
    else if (is(cmd, "door_toggle"))  { W.s.door_open = !W.s.door_open; }
    else if (is(cmd, "power_off"))    { W.s.grid_power = 0; }
    else if (is(cmd, "power_on"))     { W.s.grid_power = 1; }
    else if (is(cmd, "power_toggle")) { W.s.grid_power = !W.s.grid_power; }
    else if (is(cmd, "fault_on"))     { W.s.compressor_fault = 1; }
    else if (is(cmd, "fault_off"))    { W.s.compressor_fault = 0; }
    else if (is(cmd, "backup_on"))    { W.s.backup_on = W.s.battery_pct > 0.0; }
    else if (is(cmd, "backup_off"))   { W.s.backup_on = 0; }
    else if (is(cmd, "stress_on") || is(cmd, "stress_off") || is(cmd, "stress_toggle")) {
        int on = is(cmd, "stress_on") ? 1 : is(cmd, "stress_off") ? 0 : !W.stress_on;
        pthread_mutex_unlock(&W.lock);
        cc_stress_set(on);
        pthread_mutex_lock(&W.lock);
    }
    else if (is(cmd, "reset")) {
        W.s.door_open = 0; W.s.grid_power = 1; W.s.compressor_fault = 0;
        W.s.backup_on = 0; W.s.battery_pct = 100.0; W.s.temp_c = 4.6; W.t_true = 4.6;
        W.s.door_open_s = 0;
        cc_timing_reset(&W.g.timing);
        cc_timing_reset(&W.g.naive);
        pthread_mutex_unlock(&W.lock);
        cc_stress_set(0);
        pthread_mutex_lock(&W.lock);
    }
    else if (is(cmd, "reset_stats")) {
        cc_timing_reset(&W.g.timing);
        cc_timing_reset(&W.g.naive);
    }
    else if (is(cmd, "status") || cmd[0] == 0) { /* no-op */ }
    else ok = 0;

    if (reply && replylen > 0) {
        if (ok)
            snprintf(reply, replylen, "temp=%.2f door=%d power=%d comp=%d backup=%d batt=%.0f level=%d\n",
                     W.s.temp_c, W.s.door_open, W.s.grid_power, W.s.compressor_on,
                     W.s.backup_on, W.s.battery_pct, (int)W.g.level);
        else
            snprintf(reply, replylen, "unknown command: %s\n", cmd);
    }
    pthread_mutex_unlock(&W.lock);

    if (ok && cmd[0] && !is(cmd, "status")) {
        cc_trace(CC_TR_CMD, "cmd %s", cmd);
        if (!is(cmd, "backup_on") && !is(cmd, "backup_off") && !is(cmd, "reset_stats"))
            cc_event('i', "command: %s", cmd);
    }
    return ok ? 0 : -1;
}

static int timing_json(char *b, int len, const cc_timing_t *t)
{
    return snprintf(b, len,
        "{\"ticks\":%llu,\"period_us\":%.0f,\"jitter_us\":%.1f,\"max_jitter_us\":%.1f,"
        "\"win_max_us\":%.1f,\"exec_us\":%.1f,\"exec_max_us\":%.1f,\"misses\":%llu,"
        "\"hist\":[%u,%u,%u,%u,%u,%u,%u,%u]}",
        (unsigned long long)t->ticks, t->period_us, t->last_jitter_us, t->max_jitter_us,
        t->win_max_us, t->exec_us, t->exec_max_us, (unsigned long long)t->misses,
        t->hist[0], t->hist[1], t->hist[2], t->hist[3], t->hist[4], t->hist[5], t->hist[6], t->hist[7]);
}

static const char *lvl(cc_level_t l)
{
    return l == CC_ALARM ? "ALARM" : l == CC_WATCH ? "WATCH" : "SAFE";
}

int cc_snapshot_json(char *b, int len)
{
    cc_world_t c;
    int n, i, first = 1;
    unsigned start;

    pthread_mutex_lock(&W.lock);
    c = W;                                   /* copy, then format unlocked */
    pthread_mutex_unlock(&W.lock);
    pthread_mutex_lock(&ev_lock);
    memcpy(c.ev, W.ev, sizeof c.ev);
    c.ev_head = W.ev_head;
    pthread_mutex_unlock(&ev_lock);

    n = snprintf(b, len,
        "{\"v\":\"" CC_VERSION "\",\"platform\":\"%s\",\"qnx\":%s,\"t\":%.3f,"
        "\"temp\":%.3f,\"ambient\":%.1f,\"door\":%d,\"door_s\":%.1f,\"power\":%d,"
        "\"compressor\":%d,\"backup\":%d,\"fault\":%d,\"battery\":%.1f,"
        "\"level\":\"%s\",\"slope\":%.3f,\"ttb\":%.1f,\"buzzer\":%d,"
        "\"stress\":%d,\"stress_threads\":%d,\"cpus\":%d,\"seq\":%llu,\"guardian\":",
        c.platform, c.qnx_native ? "true" : "false", cc_now_s() - g_t0,
        c.s.temp_c, c.s.ambient_c, c.s.door_open, c.s.door_open_s, c.s.grid_power,
        c.s.compressor_on, c.s.backup_on, c.s.compressor_fault, c.s.battery_pct,
        lvl(c.g.level), c.g.slope_c_per_min, c.g.ttb_s, c.g.buzzer,
        c.stress_on, c.stress_threads, cc_ncpu(), (unsigned long long)c.s.seq);
    n += timing_json(b + n, len - n, &c.g.timing);
    n += snprintf(b + n, len - n, ",\"naive\":");
    n += timing_json(b + n, len - n, &c.g.naive);
    n += snprintf(b + n, len - n, ",\"events\":[");

    start = c.ev_head > 10 ? c.ev_head - 10 : 0;
    for (i = (int)start; i < (int)c.ev_head && n < len - 200; i++) {
        const cc_event_t *e = &c.ev[i % CC_EVENTS];
        char esc[200];
        int k = 0;
        for (const char *p = e->msg; *p && k < (int)sizeof esc - 2; p++) {
            if (*p == '"' || *p == '\\') esc[k++] = '\\';
            esc[k++] = *p;
        }
        esc[k] = 0;
        n += snprintf(b + n, len - n, "%s{\"id\":%d,\"t\":%.2f,\"l\":\"%c\",\"m\":\"%s\"}",
                      first ? "" : ",", i, e->t, e->level, esc);
        first = 0;
    }
    n += snprintf(b + n, len - n, "]}");
    return n;
}
