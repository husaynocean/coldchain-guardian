/*
 * "Flood CPU": one busy thread per core (+1) at priority 11 — ordinary
 * application load that happens to outrank the naive monitor (10) by one
 * level. On a priority-preemptive kernel that is enough to starve the naive
 * loop completely, while the guardian (60), the sensor driver (50), the
 * plant (20) and the web server (15) are untouched.
 *
 * Anything at priority 10 (including your ssh shell) is starved too, so the
 * flood stops itself after CC_STRESS_MAX_S seconds.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "cc.h"
#include <sched.h>
#include <stdatomic.h>
#include <unistd.h>

static atomic_int g_gen;     /* bump to stop the current generation */

static void *hog(void *arg)
{
    int my = (int)(long)arg;
    volatile double x = 1.0;
    while (atomic_load_explicit(&g_gen, memory_order_relaxed) == my) {
        for (int i = 0; i < 20000; i++) x = x * 1.0000001 + 0.0000001;
    }
    return NULL;
}

/* runs above the hogs so it can always end the flood */
static void *watchdog(void *arg)
{
    int my = (int)(long)arg;
    for (int i = 0; i < CC_STRESS_MAX_S * 10; i++) {
        usleep(100000);
        if (atomic_load(&g_gen) != my) return NULL;
    }
    cc_event('i', "CPU flood auto-stop after %d s", CC_STRESS_MAX_S);
    cc_stress_set(0);
    return NULL;
}

void cc_stress_set(int on)
{
    int n = 0;
    pthread_mutex_lock(&W.lock);
    if (on == W.stress_on) { pthread_mutex_unlock(&W.lock); return; }
    W.stress_on = on;
    pthread_mutex_unlock(&W.lock);

    int gen = atomic_fetch_add(&g_gen, 1) + 1;   /* old hogs see a new gen and exit */
    if (on) {
        int want = cc_ncpu() + 1;
        pthread_t t;
        cc_thread(&t, CC_PRIO_HTTP, SCHED_RR, watchdog, (void *)(long)gen);
        for (int i = 0; i < want; i++)
            if (cc_thread(&t, CC_PRIO_STRESS, SCHED_RR, hog, (void *)(long)gen) == 0) n++;
        cc_event('w', "CPU flood: %d busy threads at prio %d", n, CC_PRIO_STRESS);
    } else {
        cc_event('i', "CPU flood stopped");
    }
    pthread_mutex_lock(&W.lock);
    W.stress_threads = n;
    pthread_mutex_unlock(&W.lock);
}
