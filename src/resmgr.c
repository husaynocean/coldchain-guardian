/*
 * /dev/coldchain — the sensor driver, written as a QNX resource manager.
 *
 * On QNX everything is a message. The guardian doesn't peek at shared
 * memory: it calls devctl(DCMD_CC_GET_SAMPLE) on /dev/coldchain, which is
 * a synchronous MsgSend() to this server. Because QNX channels inherit
 * priority, the resource-manager thread runs the request at the guardian's
 * priority (60), not its own (50) — the sensor read cannot be starved by
 * anything the guardian outranks.
 *
 * From a QNX shell you can also drive the plant directly:
 *     cat /dev/coldchain                 # one-line status
 *     echo power_off > /dev/coldchain    # simulate grid failure
 *
 * On non-QNX hosts this file compiles to a direct, lock-protected copy so
 * the rest of the program is unchanged.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "cc.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>

#ifdef __QNX__
#include <devctl.h>
#include <fcntl.h>
#include <sched.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/iofunc.h>
#include <sys/dispatch.h>

#define DCMD_CC_GET_SAMPLE  __DIOF(_DCMD_MISC, 0x31, cc_sample_t)

static resmgr_connect_funcs_t connect_funcs;
static resmgr_io_funcs_t      io_funcs;
static iofunc_attr_t          attr;
static dispatch_t            *dpp;

/* single dispatch thread, so one static reply buffer is safe */
static char read_buf[256];

static int io_read(resmgr_context_t *ctp, io_read_t *msg, RESMGR_OCB_T *ocb)
{
    int status, len;
    size_t nbytes;

    if ((status = iofunc_read_verify(ctp, msg, ocb, NULL)) != EOK)
        return status;
    if ((msg->i.xtype & _IO_XTYPE_MASK) != _IO_XTYPE_NONE)
        return ENOSYS;

    if (ocb->offset > 0) {              /* one line per open(): then EOF */
        _IO_SET_READ_NBYTES(ctp, 0);
        return _RESMGR_NPARTS(0);
    }
    cc_command("status", read_buf, sizeof read_buf);
    len = (int)strlen(read_buf);
    nbytes = _IO_READ_GET_NBYTES(msg);
    if (nbytes > (size_t)len) nbytes = (size_t)len;

    SETIOV(ctp->iov, read_buf, nbytes);
    _IO_SET_READ_NBYTES(ctp, nbytes);
    ocb->offset += nbytes;
    return _RESMGR_NPARTS(1);
}

static int io_write(resmgr_context_t *ctp, io_write_t *msg, RESMGR_OCB_T *ocb)
{
    int status;
    size_t nbytes;
    char buf[64];

    if ((status = iofunc_write_verify(ctp, msg, ocb, NULL)) != EOK)
        return status;
    if ((msg->i.xtype & _IO_XTYPE_MASK) != _IO_XTYPE_NONE)
        return ENOSYS;

    nbytes = _IO_WRITE_GET_NBYTES(msg);
    if (nbytes > ctp->info.srcmsglen - ctp->offset - sizeof(io_write_t))
        return EBADMSG;

    size_t take = nbytes < sizeof buf - 1 ? nbytes : sizeof buf - 1;
    if (resmgr_msgget(ctp, buf, take, sizeof(msg->i)) == -1)
        return errno;
    buf[take] = 0;

    if (cc_command(buf, NULL, 0) != 0)
        return EINVAL;

    _IO_SET_WRITE_NBYTES(ctp, nbytes);
    return _RESMGR_NPARTS(0);
}

static int io_devctl(resmgr_context_t *ctp, io_devctl_t *msg, RESMGR_OCB_T *ocb)
{
    int status;
    size_t nbytes = 0;
    void *out;

    if ((status = iofunc_devctl_default(ctp, msg, ocb)) != _RESMGR_DEFAULT)
        return status;

    out = (char *)&msg->o + sizeof(msg->o);   /* reply payload follows header */

    switch (msg->i.dcmd) {
    case DCMD_CC_GET_SAMPLE:
        pthread_mutex_lock(&W.lock);
        memcpy(out, &W.s, sizeof(cc_sample_t));
        pthread_mutex_unlock(&W.lock);
        nbytes = sizeof(cc_sample_t);
        break;
    default:
        return ENOSYS;
    }

    memset(&msg->o, 0, sizeof(msg->o));
    msg->o.ret_val = 0;
    msg->o.nbytes = nbytes;
    return _RESMGR_PTR(ctp, &msg->o, sizeof(msg->o) + nbytes);
}

static void *resmgr_loop(void *arg)
{
    dispatch_context_t *ctp = dispatch_context_alloc(dpp);
    (void)arg;
    if (!ctp) { perror("dispatch_context_alloc"); return NULL; }
    for (;;) {
        if ((ctp = dispatch_block(ctp)) == NULL) {
            perror("dispatch_block");
            return NULL;
        }
        dispatch_handler(ctp);
    }
    return NULL;
}

int cc_resmgr_start(void)
{
    resmgr_attr_t rattr;
    pthread_t t;

    if ((dpp = dispatch_create()) == NULL) {
        perror("dispatch_create");
        return -1;
    }
    memset(&rattr, 0, sizeof rattr);
    rattr.nparts_max   = 1;
    rattr.msg_max_size = 2048;

    iofunc_func_init(_RESMGR_CONNECT_NFUNCS, &connect_funcs, _RESMGR_IO_NFUNCS, &io_funcs);
    io_funcs.read   = io_read;
    io_funcs.read64 = io_read;
    io_funcs.write  = io_write;
    io_funcs.devctl = io_devctl;
    iofunc_attr_init(&attr, S_IFCHR | 0666, NULL, NULL);

    if (resmgr_attach(dpp, &rattr, CC_DEVICE_PATH, _FTYPE_ANY, 0,
                      &connect_funcs, &io_funcs, &attr) == -1) {
        perror("resmgr_attach " CC_DEVICE_PATH " (run as root)");
        return -1;
    }
    cc_thread(&t, CC_PRIO_RESMGR, SCHED_RR, resmgr_loop, NULL);
    cc_event('i', "resource manager up: " CC_DEVICE_PATH);
    return 0;
}

int cc_sensor_open(void)
{
    return open(CC_DEVICE_PATH, O_RDWR);
}

int cc_sensor_read(int fd, cc_sample_t *out)
{
    /* one MsgSend() round trip, priority inherited by the server */
    return devctl(fd, DCMD_CC_GET_SAMPLE, out, sizeof *out, NULL);
}

#else  /* ---------------- non-QNX development fallback ---------------- */

int cc_resmgr_start(void)
{
    cc_event('i', "dev mode: no resource manager (not QNX)");
    return 0;
}

int cc_sensor_open(void) { return 0; }

int cc_sensor_read(int fd, cc_sample_t *out)
{
    (void)fd;
    pthread_mutex_lock(&W.lock);
    *out = W.s;
    pthread_mutex_unlock(&W.lock);
    return 0;
}
#endif
