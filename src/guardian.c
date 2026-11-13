/*
 * The guardian: a priority-60 control loop woken every 5 ms by a kernel
 * timer pulse. Each tick it
 *   1. reads the sensor through /dev/coldchain (devctl → MsgSend)
 *   2. measures its own wake-up jitter with ClockCycles()
 *   3. every 100 ms, fits a least-squares trend to the last 4 s of readings
 *      and predicts the time until vials leave the 2–8 °C band
 *   4. acts: engages battery cooling before the breach, sounds the door
 *      buzzer, and logs every decision as a QNX trace event
 *
 * A second, "naive" monitor runs the same 5 ms schedule at priority 10.
 * When the UI floods the CPU with priority-10 work, the naive monitor's
 * jitter explodes while the guardian's stays flat — that contrast is the
 * whole argument for a real-time OS.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "cc.h"
#include <math.h>
#include <sched.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#ifdef __QNX__
#include <sys/neutrino.h>
#include <sys/siginfo.h>
#include <signal.h>
#define PULSE_TICK (_PULSE_CODE_MINAVAIL + 1)
#endif

#define HIST_N        40      /* 40 samples x 100 ms = 4 s trend window */
#define PREDICT_EVERY 20      /* ticks between trend fits (100 ms)      */
#define BACKUP_LEAD_S 60.0    /* engage battery cooling this early      */
#define DOOR_BUZZ_S   15.0

void cc_timing_reset(cc_timing_t *t)
{
    memset(t, 0, sizeof *t);
    t->period_us = CC_GUARD_PERIOD_US;
}

/* rolling 1 s window max, kept per thread */
typedef struct { double cur, prev; uint64_t n; } winmax_t;

static void timing_update(cc_timing_t *t, winmax_t *w, double interval_us, double exec_us)
{
    double j = fabs(interval_us - CC_GUARD_PERIOD_US);
    static const double edges[7] = { 10, 25, 50, 100, 250, 1000, 5000 };
    int b = 0;

    while (b < 7 && j >= edges[b]) b++;
    t->hist[b]++;
    t->ticks++;
    t->last_s = cc_now_s();
    t->last_jitter_us = j;
    if (j > t->max_jitter_us) t->max_jitter_us = j;
    if (j > CC_GUARD_DEADLINE_US) t->misses++;
    t->exec_us = exec_us;
    if (exec_us > t->exec_max_us) t->exec_max_us = exec_us;

    if (j > w->cur) w->cur = j;
    if (++w->n % (1000000 / CC_GUARD_PERIOD_US) == 0) { w->prev = w->cur; w->cur = 0; }
    t->win_max_us = w->cur > w->prev ? w->cur : w->prev;
}

/* least-squares slope in °C per second over the ring buffer */
static double trend(const double *h, int n, double dt)
{
    double sx = 0, sy = 0, sxx = 0, sxy = 0;
    for (int i = 0; i < n; i++) {
        double x = i * dt;
        sx += x; sy += h[i]; sxx += x * x; sxy += x * h[i];
    }
    double d = n * sxx - sx * sx;
    return d == 0 ? 0 : (n * sxy - sx * sy) / d;
}

static void actuate(int fd, const char *cmd)
{
#ifdef __QNX__
    /* actuation goes through the driver too: a write() is a MsgSend() */
    if (write(fd, cmd, strlen(cmd)) < 0) perror("actuate");
#else
    (void)fd;
    cc_command(cmd, NULL, 0);
#endif
}

static const char *lname(cc_level_t l) { return l == CC_ALARM ? "ALARM" : l == CC_WATCH ? "WATCH" : "SAFE"; }

void *cc_guardian_thread(void *arg)
{
    int fd;
    cc_sample_t s;
    double hist[HIST_N];
    int hn = 0, hpos = 0;
    uint64_t tick = 0, last_c = 0;
    winmax_t wm = { 0 };
    cc_level_t level = CC_SAFE;
    double slope = 0, ttb = -1;
    int buzzer = 0;
    char num[32];
    (void)arg;

    fd = cc_sensor_open();
    if (fd < 0) { perror("open " CC_DEVICE_PATH); return NULL; }

#ifdef __QNX__
    /* kernel timer → pulse on a private channel: no signals, no polling */
    int chid = ChannelCreate(0);
    int coid = ConnectAttach(0, 0, chid, _NTO_SIDE_CHANNEL, 0);
    struct sigevent ev;
    timer_t tid;
    struct itimerspec its;
    struct _pulse pulse;

    SIGEV_PULSE_INIT(&ev, coid, CC_PRIO_GUARDIAN, PULSE_TICK, 0);
    if (timer_create(CLOCK_MONOTONIC, &ev, &tid) == -1) { perror("timer_create"); return NULL; }
    its.it_value.tv_sec = 0;
    its.it_value.tv_nsec = CC_GUARD_PERIOD_US * 1000L;
    its.it_interval = its.it_value;
    timer_settime(tid, 0, &its, NULL);
#else
    struct timespec next;
    clock_gettime(CLOCK_MONOTONIC, &next);
#endif

    cc_event('g', "guardian online: prio %d, %d ms period", CC_PRIO_GUARDIAN, CC_GUARD_PERIOD_US / 1000);

    for (;;) {
#ifdef __QNX__
        if (MsgReceivePulse(chid, &pulse, sizeof pulse, NULL) == -1) continue;
#else
        next.tv_nsec += CC_GUARD_PERIOD_US * 1000L;
        if (next.tv_nsec >= 1000000000L) { next.tv_nsec -= 1000000000L; next.tv_sec++; }
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL);
#endif
        uint64_t c0 = cc_cycles();
        double interval_us = last_c ? cc_cycles_to_us(c0 - last_c) : CC_GUARD_PERIOD_US;
        last_c = c0;
        tick++;

        if (cc_sensor_read(fd, &s) != 0) continue;

        /* ---- prediction, every 100 ms ---- */
        if (tick % PREDICT_EVERY == 0) {
            hist[hpos] = s.temp_c;
            hpos = (hpos + 1) % HIST_N;
            if (hn < HIST_N) hn++;
            if (hn >= 10) {
                double ordered[HIST_N];
                for (int i = 0; i < hn; i++)
                    ordered[i] = hist[(hpos - hn + i + HIST_N) % HIST_N];
                slope = trend(ordered, hn, PREDICT_EVERY * CC_GUARD_PERIOD_US / 1e6);
            }
            /* Only predict when the trend is not going to be corrected by the
             * thermostat: cooling unavailable, door open, or temperature
             * still climbing above the compressor cut-in point. */
            int cooling_ok = (s.grid_power && !s.compressor_fault) || s.backup_on;
            int runaway = !cooling_ok || s.door_open || s.temp_c > 5.4;
            ttb = -1;
            if (s.temp_c <= CC_SAFE_MAX_C && slope > 0.002 && runaway)
                ttb = (CC_SAFE_MAX_C - s.temp_c) / slope;
            else if (s.temp_c >= CC_SAFE_MIN_C && s.temp_c < 2.8 && slope < -0.002)
                ttb = (s.temp_c - CC_SAFE_MIN_C) / -slope;

            cc_level_t nl = CC_SAFE;
            if (s.temp_c > CC_SAFE_MAX_C || s.temp_c < CC_SAFE_MIN_C) nl = CC_ALARM;
            else if ((ttb >= 0 && ttb < 120) || s.door_open_s > DOOR_BUZZ_S || !s.grid_power) nl = CC_WATCH;

            if (nl != level) {
                snprintf(num, sizeof num, "%.2f", s.temp_c);
                cc_trace(CC_TR_STATE, "%s -> %s T=%s", lname(level), lname(nl), num);
                cc_event(nl == CC_ALARM ? 'a' : nl == CC_WATCH ? 'w' : 'i',
                         "%s → %s at %s °C", lname(level), lname(nl), num);
                level = nl;
            }

            /* ---- actions ---- */
            if (!s.grid_power && !s.backup_on && s.battery_pct > 1.0 &&
                ((ttb >= 0 && ttb < BACKUP_LEAD_S) || s.temp_c > 6.5)) {
                actuate(fd, "backup_on");
                snprintf(num, sizeof num, "%.0f", ttb);
                cc_trace(CC_TR_ACTION, "backup_on ttb=%s", num);
                cc_event('g', "grid lost — battery cooling engaged %s s before breach", num);
            }
            if (s.grid_power && s.backup_on) {
                actuate(fd, "backup_off");
                cc_trace(CC_TR_ACTION, "backup_off");
                cc_event('g', "grid restored — battery cooling released");
            }
            int want_buzz = s.door_open_s > DOOR_BUZZ_S;
            if (want_buzz != buzzer) {
                buzzer = want_buzz;
                cc_trace(CC_TR_ACTION, "buzzer %d", buzzer);
                if (buzzer) cc_event('g', "door open > %.0f s — buzzer on, pharmacist alerted", DOOR_BUZZ_S);
            }
        }

        double exec_us = cc_cycles_to_us(cc_cycles() - c0);

        pthread_mutex_lock(&W.lock);
        timing_update(&W.g.timing, &wm, interval_us, exec_us);
        W.g.level = level;
        W.g.slope_c_per_min = slope * 60.0;
        W.g.ttb_s = ttb;
        W.g.buzzer = buzzer;
        pthread_mutex_unlock(&W.lock);

        if (fabs(interval_us - CC_GUARD_PERIOD_US) > CC_GUARD_DEADLINE_US) {
            snprintf(num, sizeof num, "%.0f", interval_us);
            cc_trace(CC_TR_MISS, "guardian late interval_us=%s", num);
        }
    }
    return NULL;
}

/* Same 5 ms schedule, no privileges: what most IoT firmware looks like. */
void *cc_naive_thread(void *arg)
{
    struct timespec next;
    uint64_t last_c = 0;
    winmax_t wm = { 0 };
    (void)arg;

    clock_gettime(CLOCK_MONOTONIC, &next);
    for (;;) {
        next.tv_nsec += CC_GUARD_PERIOD_US * 1000L;
        if (next.tv_nsec >= 1000000000L) { next.tv_nsec -= 1000000000L; next.tv_sec++; }
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL);

        uint64_t c0 = cc_cycles();
        double interval_us = last_c ? cc_cycles_to_us(c0 - last_c) : CC_GUARD_PERIOD_US;
        last_c = c0;

        /* if we overslept several periods, resynchronise instead of bursting */
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        long long late_ns = (long long)(now.tv_sec - next.tv_sec) * 1000000000LL
                          + (now.tv_nsec - next.tv_nsec);
        if (late_ns > CC_GUARD_PERIOD_US * 1000LL) next = now;

        pthread_mutex_lock(&W.lock);
        timing_update(&W.g.naive, &wm, interval_us, 0);
        pthread_mutex_unlock(&W.lock);
    }
    return NULL;
}
