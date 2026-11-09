/*
 * coldchaind — Coldchain Guardian daemon for QNX 8.0
 *
 *   coldchaind [--port 8080] [--web /data/coldchain/web]
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "cc.h"
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

static void usage(const char *a0)
{
    fprintf(stderr,
        "usage: %s [--port N] [--web DIR]\n"
        "  --port N   dashboard API port (default 8080)\n"
        "  --web DIR  also serve the dashboard from DIR\n", a0);
}

int main(int argc, char **argv)
{
    int port = 8080;
    const char *web = NULL;
    pthread_t t;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--port") && i + 1 < argc) port = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--web") && i + 1 < argc) web = argv[++i];
        else { usage(argv[0]); return 1; }
    }

    signal(SIGPIPE, SIG_IGN);
    /* real-time hygiene: no page faults in the control path */
    if (mlockall(MCL_CURRENT | MCL_FUTURE) != 0)
        fprintf(stderr, "note: mlockall unavailable, continuing\n");

    cc_world_init();
    fprintf(stderr, "Coldchain Guardian %s on %s\n", CC_VERSION, W.platform);

    if (cc_resmgr_start() != 0) return 1;
    cc_thread(&t, CC_PRIO_PLANT,    SCHED_FIFO, cc_plant_thread,    NULL);
    cc_thread(&t, CC_PRIO_GUARDIAN, SCHED_FIFO, cc_guardian_thread, NULL);
    cc_thread(&t, CC_PRIO_NAIVE,    SCHED_RR,   cc_naive_thread,    NULL);
    if (cc_http_start(port, web) != 0) return 1;

    for (;;) pause();
    return 0;
}
