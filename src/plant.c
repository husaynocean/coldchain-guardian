/*
 * The "plant": a lumped thermal model of a vaccine refrigerator in a hot
 * room. Constants are tuned so events happen on demo time-scales (a grid
 * failure takes ~80 s to push vials past 8 °C, an open door ~10 s).
 *
 *   dT/dt = k_leak (T_amb - T) + k_door·door (T_amb - T) - cooling
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "cc.h"
#include <stdlib.h>
#include <time.h>

#define K_LEAK        0.0016   /* 1/s, insulation losses          */
#define K_DOOR        0.0120   /* 1/s, extra exchange, door open   */
#define COOL_MAINS    0.090    /* °C/s, AC compressor              */
#define COOL_BACKUP   0.060    /* °C/s, DC battery compressor      */
#define TSTAT_ON      5.0      /* compressor cut-in                */
#define TSTAT_OFF     3.5      /* compressor cut-out               */
#define BATT_DRAIN    0.45     /* %/s while backup compressor runs */
#define BATT_CHARGE   0.30     /* %/s while on grid power          */
#define PLANT_DT_NS   10000000L

static double noise(void)
{
    /* cheap triangular noise in ±0.012 °C */
    return ((double)rand() / RAND_MAX - (double)rand() / RAND_MAX) * 0.012;
}

void *cc_plant_thread(void *arg)
{
    struct timespec next;
    double last = cc_now_s();
    (void)arg;

    clock_gettime(CLOCK_MONOTONIC, &next);
    for (;;) {
        double now, dt, T, dT, amb;
        cc_sample_t *s = &W.s;

        next.tv_nsec += PLANT_DT_NS;
        if (next.tv_nsec >= 1000000000L) { next.tv_nsec -= 1000000000L; next.tv_sec++; }
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL);

        now = cc_now_s();
        dt = now - last;
        last = now;

        pthread_mutex_lock(&W.lock);
        dt *= W.sim_speed;
        T = W.t_true;
        amb = s->ambient_c;

        /* thermostat with hysteresis — runs on whichever source is live */
        int can_mains  = s->grid_power && !s->compressor_fault;
        int can_backup = !s->grid_power && s->backup_on && s->battery_pct > 0.0;
        if (can_mains || can_backup) {
            if (T > TSTAT_ON)  s->compressor_on = 1;
            if (T < TSTAT_OFF) s->compressor_on = 0;
        } else {
            s->compressor_on = 0;
        }

        dT = K_LEAK * (amb - T);
        if (s->door_open) dT += K_DOOR * (amb - T);
        if (s->compressor_on) dT -= can_mains ? COOL_MAINS : COOL_BACKUP;

        T += dT * dt;
        if (T > amb) T = amb;
        if (T < -4.0) T = -4.0;
        W.t_true = T;
        s->temp_c = T + noise();       /* sensor noise, not plant noise */

        if (s->door_open) s->door_open_s += dt; else s->door_open_s = 0;

        if (can_backup && s->compressor_on) {
            s->battery_pct -= BATT_DRAIN * dt;
            if (s->battery_pct <= 0.0) { s->battery_pct = 0.0; s->backup_on = 0; }
        }
        if (s->grid_power && s->battery_pct < 100.0) {
            s->battery_pct += BATT_CHARGE * dt;
            if (s->battery_pct > 100.0) s->battery_pct = 100.0;
        }
        s->seq++;
        pthread_mutex_unlock(&W.lock);
    }
    return NULL;
}
