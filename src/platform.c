/*
 * Platform layer. On QNX we use the native primitives (ClockCycles, the
 * system page, trace_logf). On Linux the same code compiles with POSIX
 * fallbacks so the dashboard can be developed without a target — but the
 * timing guarantees only exist on QNX.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "cc.h"
#include <errno.h>
#include <sched.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#ifdef __QNX__
#include <sys/neutrino.h>
#include <sys/syspage.h>
#include <sys/trace.h>
#endif

double cc_now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

#ifdef __QNX__
static double g_cps;   /* cycles per second, from the system page */

uint64_t cc_cycles(void) { return ClockCycles(); }

double cc_cycles_to_us(uint64_t c)
{
    if (g_cps == 0.0) g_cps = (double)SYSPAGE_ENTRY(qtime)->cycles_per_sec;
    return (double)c * 1e6 / g_cps;
}
#else
uint64_t cc_cycles(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}
double cc_cycles_to_us(uint64_t c) { return (double)c / 1000.0; }
#endif

int cc_set_my_prio(int prio, int policy)
{
    struct sched_param sp;
    memset(&sp, 0, sizeof sp);
    sp.sched_priority = prio;
    return pthread_setschedparam(pthread_self(), policy, &sp);
}

/*
 * Create a thread with an explicit scheduling policy and priority.
 * On Linux without CAP_SYS_NICE this falls back to a default thread.
 */
int cc_thread(pthread_t *t, int prio, int policy, void *(*fn)(void *), void *arg)
{
    pthread_attr_t a;
    struct sched_param sp;
    int rc;

    pthread_attr_init(&a);
    pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
    pthread_attr_setinheritsched(&a, PTHREAD_EXPLICIT_SCHED);
    pthread_attr_setschedpolicy(&a, policy);
    memset(&sp, 0, sizeof sp);
    sp.sched_priority = prio;
    pthread_attr_setschedparam(&a, &sp);

    rc = pthread_create(t, &a, fn, arg);
    pthread_attr_destroy(&a);
    if (rc == EPERM || rc == EINVAL) {
        /* not privileged (typical on a Linux dev box): run unprioritised */
        pthread_attr_init(&a);
        pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
        rc = pthread_create(t, &a, fn, arg);
        pthread_attr_destroy(&a);
    }
    return rc;
}

/*
 * User trace events. On QNX these land in the kernel event stream next to
 * every context switch, so `tracelogger` + `traceprinter` (or the Momentics
 * System Profiler) show exactly when the guardian ran and what it decided.
 * trace_logf() does not support %f, so callers pre-format numbers.
 */
void cc_trace(int code, const char *fmt, ...)
{
    char buf[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
#ifdef __QNX__
    trace_logf(_NTO_TRACE_USERFIRST + code, "%s", buf);
#else
    (void)code;
#endif
}

int cc_ncpu(void)
{
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return n > 0 ? (int)n : 1;
}
