/*
 * Coldchain Guardian — shared types and interfaces.
 *
 * Process layout (one process, several threads, each with an explicit
 * real-time priority):
 *
 *   prio 60  guardian      pulse-driven 5 ms control loop (the safety thread)
 *   prio 50  resmgr        /dev/coldchain resource manager (inherits client prio)
 *   prio 20  plant         vaccine-fridge thermal simulation (10 ms)
 *   prio 15  http          REST + Server-Sent Events for the 3D dashboard
 *   prio 10  naive         the same check as the guardian, at "normal" priority
 *   prio 10  stress x N    CPU hogs, toggled from the UI ("Flood CPU")
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef CC_H
#define CC_H

#include <stdint.h>
#include <pthread.h>

#define CC_VERSION        "1.0.0"
#define CC_DEVICE_PATH    "/dev/coldchain"

/* WHO safe band for most vaccines */
#define CC_SAFE_MIN_C     2.0
#define CC_SAFE_MAX_C     8.0

#define CC_PRIO_GUARDIAN  60
#define CC_PRIO_RESMGR    50
#define CC_PRIO_PLANT     20
#define CC_PRIO_HTTP      15
#define CC_PRIO_NAIVE     10
#define CC_PRIO_STRESS    10

#define CC_GUARD_PERIOD_US   5000   /* guardian tick: 5 ms  */
#define CC_GUARD_DEADLINE_US 1000   /* must wake within 1 ms of its slot */

/* ---- sample published by the sensor driver (/dev/coldchain devctl) ---- */
typedef struct {
    uint64_t seq;            /* increments every plant step              */
    double   temp_c;         /* interior air temperature                 */
    double   ambient_c;      /* room temperature                         */
    uint8_t  door_open;
    uint8_t  grid_power;     /* mains / generator present                */
    uint8_t  compressor_on;
    uint8_t  backup_on;      /* battery-backed cooling engaged           */
    uint8_t  compressor_fault;
    uint8_t  _pad[3];
    double   battery_pct;
    double   door_open_s;    /* how long the door has been open          */
} cc_sample_t;

/* ---- guardian verdict ---- */
typedef enum { CC_SAFE = 0, CC_WATCH = 1, CC_ALARM = 2 } cc_level_t;

typedef struct {
    uint64_t ticks;
    double   period_us;
    double   last_jitter_us;   /* |actual interval - period|            */
    double   max_jitter_us;    /* since reset                           */
    double   win_max_us;       /* max over the last second              */
    double   exec_us;          /* time spent in one loop iteration      */
    double   exec_max_us;
    uint64_t misses;           /* wake-ups later than the deadline      */
    uint32_t hist[8];          /* <10,<25,<50,<100,<250,<1000,<5000,>=5000 us */
} cc_timing_t;

typedef struct {
    cc_level_t level;
    double     slope_c_per_min;
    double     ttb_s;          /* predicted seconds to breach, <0 = none */
    int        buzzer;
    cc_timing_t timing;        /* guardian (prio 60)                     */
    cc_timing_t naive;         /* naive monitor (prio 10)                */
} cc_guard_t;

/* ---- event journal ---- */
#define CC_EVENTS 32
typedef struct {
    double t;                  /* seconds since start */
    char   level;              /* 'i' info, 'w' watch, 'a' alarm, 'g' guardian action */
    char   msg[96];
} cc_event_t;

/* ---- world: everything, guarded by one priority-inheriting mutex ---- */
typedef struct {
    pthread_mutex_t lock;
    cc_sample_t     s;
    double          t_true;    /* plant state; s.temp_c = t_true + sensor noise */
    cc_guard_t      g;
    int             stress_on;
    int             stress_threads;
    double          sim_speed;
    cc_event_t      ev[CC_EVENTS];
    unsigned        ev_head;   /* total events ever written */
    char            platform[96];
    int             qnx_native;
} cc_world_t;

extern cc_world_t W;

/* platform.c */
double   cc_now_s(void);            /* monotonic seconds                     */
uint64_t cc_cycles(void);           /* high-res counter (ClockCycles on QNX) */
double   cc_cycles_to_us(uint64_t);
int      cc_thread(pthread_t *t, int prio, int policy, void *(*fn)(void *), void *arg);
int      cc_set_my_prio(int prio, int policy);
void     cc_trace(int code, const char *fmt, ...);
int      cc_ncpu(void);

/* world.c */
void cc_world_init(void);
void cc_event(char level, const char *fmt, ...);
int  cc_command(const char *cmd, char *reply, int replylen); /* text command API */
int  cc_snapshot_json(char *buf, int len);

/* plant.c */
void *cc_plant_thread(void *);

/* resmgr.c */
int   cc_resmgr_start(void);             /* returns 0 when /dev/coldchain is up */
int   cc_sensor_open(void);              /* client side: open driver            */
int   cc_sensor_read(int fd, cc_sample_t *out);

/* guardian.c */
void *cc_guardian_thread(void *);
void *cc_naive_thread(void *);
void  cc_timing_reset(cc_timing_t *t);

/* stress.c */
void cc_stress_set(int on);

/* http.c */
int cc_http_start(int port, const char *webroot);

/* trace codes (user events, see tracelogger / traceprinter) */
#define CC_TR_STATE   100
#define CC_TR_ACTION  101
#define CC_TR_MISS    102
#define CC_TR_CMD     103

#endif
