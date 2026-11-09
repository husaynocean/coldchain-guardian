/*
 * "Flood CPU": one busy thread per core (+1) at priority 10, round-robin.
 * They compete with the naive monitor as equals and are simply outranked
 * by the guardian, the sensor driver, the plant and the web server.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "cc.h"
#include <sched.h>
#include <stdatomic.h>

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
        for (int i = 0; i < want; i++) {
            pthread_t t;
            if (cc_thread(&t, CC_PRIO_STRESS, SCHED_RR, hog, (void *)(long)gen) == 0) n++;
        }
        cc_event('w', "CPU flood: %d busy threads at prio %d", n, CC_PRIO_STRESS);
    } else {
        cc_event('i', "CPU flood stopped");
    }
    pthread_mutex_lock(&W.lock);
    W.stress_threads = n;
    pthread_mutex_unlock(&W.lock);
}
